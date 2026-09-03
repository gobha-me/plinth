# ICD-0.4.4-package-install-lifecycle

**Traces to:** DESIGN-packages-v04x.md §0.4.4 (Package Install Lifecycle — the authoritative state-machine contract + asset-serving route + shell first-boot); DESIGN-packages-v04x.md §4.1 (`plinth.packages` table — introduced in this milestone); DESIGN-packages-v04x.md §4.3 (`plinth.panels` table — introduced in this milestone); DESIGN-packages-v04x.md §4.4 (on-disk layout under `{data_dir}/extensions/`); DESIGN-packages-v04x.md §8 (Atomic Swap — crash-recovery sub-surface only; upgrade swap is 0.4.5); architecture/05-extensions.md §2 (Reserved URL Prefixes — `/ext/{name}/{version}/*` owner + `/api/packages` owner); architecture/06-frontend.md §3 (asset-serving headers + MIME table).
**Depends on:** ICD-0.4.0-package-structure-validation (`validate()` entry called at VALIDATING); ICD-0.4.1-manifest-parsing (typed manifest access in-process); ICD-0.4.1-glassworm-defense (Layer 1 scan runs transitively through `validate()`); ICD-0.4.2-cross-file-manifest-validation (cross-file pass + runtime-state queries against the new `plinth.packages` table); ICD-0.4.3-extension-schema-creation-and-migration (`run_migrations()` called at MIGRATING); ICD-0.2.0-capability-registry / ICD-0.2.4-capability-rbac (bulk `register_capability()` called at REGISTERING); ICD-0.1.4-groups-rbac (`plinth.rbac_rules` INSERT called at REGISTERING; full rule validation is 0.4.6's concern, 0.4.4 ships a minimal rule-insert shim); ICD-0.1.5-rbac-enforcement (`RbacFilter` gates `POST /api/packages`).
**Milestone:** 0.4.4 — orchestrate the full install path from admin zip upload to ACTIVE handlers, plus static asset serving at `/ext/{name}/{version}/*`, plus shell first-boot self-install from bundled blob. Does NOT implement disable/enable/uninstall/upgrade (those are 0.4.5), rule validator RBAC validation (0.4.6), or RBAC tests (0.4.7).
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** `migrations/schema.sql:116` (the "Packages — Added in 0.4.0" placeholder comment — 0.4.4 materializes `plinth.packages` + `plinth.panels` here, the comment header stays where it is); `src/kernel/packages/validator.{hpp,cpp}` (called at VALIDATING); `src/kernel/packages/migrations.{hpp,cpp}` (0.4.3 surface called at MIGRATING); `src/kernel/capabilities/resolution.{hpp,cpp}` (registry entry called at REGISTERING); `src/kernel/main.cpp` (Drogon bootstrap — the asset route + admin endpoint are registered here; the shell first-boot hook runs after `bootstrap_schema` and before `app().run()`); `DEFERRED.md` (no active entries block 0.4.4; the 2026-04-18 per-op `search_path` entry is 0.5.x scope and does not gate this milestone).

---

## Overview

0.4.4 wires the package system's components into a single end-to-end path. An admin POSTs a zip to `/api/packages`; the kernel validates, migrates, registers, extracts, activates, and returns `{id, state: "ACTIVE"}`. If any stage fails, state rolls back to `INSTALL_FAILED` and the admin gets a structured error. Alongside this, the kernel gains two permanent URL surfaces: `/ext/{name}/{version}/*` for extension-asset serving, and (as a special case of the same install path) the bundled shell blob that installs itself at first boot when no frontend extension is present.

This is the orchestrator. 0.4.0–0.4.3 supply the components. 0.4.4 does the state-machine plumbing, the new Drogon routes, the new schema tables, and the shell first-boot hook. It does not introduce any new validation rule, any new migration semantic, or any new capability-registry shape — those surfaces are consumed.

**Scope:**

- **New schema tables.** `plinth.packages` (install-state record per DESIGN §4.1) and `plinth.panels` (panel registry per DESIGN §4.3). Both land in `migrations/schema.sql` in this PR — the schema file is still fluid in 0.4 (it freezes at 0.7.0).
- **New `src/kernel/packages/install_lifecycle.{hpp,cpp}`** — the state machine. Public entry `install_package(zip_blob, provenance, caller_ctx)` returning `std::expected<PackageRecord, InstallFailure>`. Drives UPLOADING through ACTIVE.
- **New `src/kernel/packages/panels.{hpp,cpp}`** — `plinth.panels` CRUD + `register_package_panels()` helper called at REGISTERING. Panel queries consumed by the shell live in a future milestone (0.6.x).
- **New `src/kernel/packages/asset_server.{hpp,cpp}`** — Drogon handler for `GET /ext/{name}/{version}/{path:.*}`. Static file serving against `{data_dir}/extensions/{name}/{version}/client/{path}`. Immutable-cache headers, MIME table, closed-domain path resolution.
- **New `src/kernel/packages/handlers.{hpp,cpp}`** — the HTTP layer. `POST /api/packages` (admin-authenticated, `packages.install` rule) accepts a `multipart/form-data` upload, writes a staging file, invokes `install_package`, returns JSON. `GET /api/packages` lists installed packages (admin-gated). `GET /api/packages/{id}` returns a single record. No delete/disable/enable in 0.4.4 — those ship with 0.4.5.
- **New `src/kernel/packages/shell_blob.{hpp,cpp}`** — access to the embedded bundled shell package. The blob itself is produced by a new CMake rule (`shell_blob.o` object file from the shell source tree) and referenced via extern symbols. On kernel boot, if `SELECT 1 FROM plinth.packages WHERE frontend_mount IS NOT NULL AND state IN ('ACTIVE', 'ACTIVE_FLAGGED')` is empty, `install_package(shell_blob, provenance=BUNDLED, caller_ctx=UserContext::kernel())` runs. Failure here aborts bootstrap.
- **Crash recovery.** New `reconcile_in_flight_installs()` called after schema bootstrap, before the shell first-boot check. Any row in `UPLOADING`, `VALIDATING`, `MIGRATING`, `REGISTERING`, `EXTRACTING`, or `ACTIVATING` at startup is evaluated against on-disk evidence and advanced to `ACTIVE` or marked `INSTALL_FAILED`.
- **PG advisory-lock per package name** wrapping the full lifecycle. Concurrent installs of the same package serialize; different packages proceed in parallel. Lock key: `hashtextextended('plinth.packages.' || name, 0)`.
- **New RBAC rules wired at kernel bootstrap.** `packages.install`, `packages.read` seeded in `bootstrap_groups` alongside the 0.1.5 defaults. 0.4.4 grants both to the built-in admin group at bootstrap time.
- **Minimal rule-insert shim.** REGISTERING stage calls `plinth::rbac::upsert_extension_rule(name, namespace, description, extension_name)` for each entry in `rbac.json`. No `test_contract` column, no rule validation — those are 0.4.6's concern. 0.4.4 surfaces RBAC rules structurally so capability dispatch can evaluate them immediately; RBAC test runs post-0.4.7.
- **`reconcile_in_flight_installs()` integration test** under `PLINTH_KERNEL_TESTS=ON` that force-kills the kernel mid-install and verifies recovery.
- **Test fixtures.** One passing end-to-end fixture (`valid-install/`), one per failure-at-stage (`fail-at-validating/`, `fail-at-migrating/`, ...). `PLINTH_KERNEL_TESTS=ON` gated.

**Out of scope (deferred):**

- **Disable, enable, uninstall, upgrade.** All ship in 0.4.5 per DESIGN §0.4.5. 0.4.4 has no transitions out of ACTIVE; the only failure state is INSTALL_FAILED. `plinth.packages.disabled_at` / `uninstalling_at` columns are present in the schema but populated only by 0.4.5.
- **Atomic-swap upgrade.** DESIGN §8 is a 0.4.5 concern. 0.4.4 rejects upload if `SELECT 1 FROM plinth.packages WHERE name = ? AND state NOT IN ('UNINSTALLING')` returns a row, even with a newer `version`. 0.4.4's "first install only" posture is deliberate — upgrade semantics need the disable path's route-drain logic that 0.4.5 brings.
- **rule validator RBAC validation.** 0.4.6. 0.4.4's rule insert is a structural pass-through; it does not validate test-contract syntax or capability existence beyond `capabilities.json` cross-checks already done by 0.4.2.
- **RBAC test RBAC integration tests.** 0.4.7. 0.4.4 leaves `plinth.packages.last_rbac_test_run_at` NULL on install.
- **`ACTIVE_FLAGGED`.** Reserved in the state enum, never entered in 0.4.4. 0.4.7 is the first writer.
- **Admin UI.** Zip upload happens via `curl -F` / `fetch()` in 0.4.4; the shell-side admin uploader ships with the admin extension (0.6a-A).
- **Retention window for old versions.** DESIGN §0.4.5 describes a 24h retention and garbage collection. 0.4.4 does not create or retain multiple versions — `{data_dir}/extensions/{name}/active` symlink exists but points only at the one installed version; its role is to enable the 0.4.5 atomic swap without a 0.4.4 rewrite.
- **Signed / remote / registry installs.** All deferred per DESIGN §7.2. Only two provenance values in 0.4.4: `user` (admin-uploaded) and `bundled` (shell blob).
- **Resumable / chunked upload.** The `/api/packages` endpoint accepts single-request `multipart/form-data` up to the configured `max_package_size_mb` (default 50 MB). Resumable is a storage-layer concern (0.10.x) per `architecture/03-data.md §2.3`.
- **Per-extension `runtime.*` clamping beyond the existing 0.3.1 ceilings.** `manifest.runtime.memory_limit_mb` etc. are read but not dynamically applied per-extension at 0.4.4 — the kernel-wide QuickJS limits from 0.3.1 continue to govern every runtime pool. Per-extension runtime clamping is a 0.3.x follow-up when the pool gains per-extension isolation (not yet scheduled).

---

## State Machine

```
   (admin POSTs zip)                         (kernel bootstrap, no frontend installed)
            │                                              │
            ▼                                              ▼
        UPLOADING ──── failure ───▶ 4xx, nothing persisted in plinth.packages
            │                                              │
            ▼                                              │
        VALIDATING ─── failure ───▶ INSTALL_FAILED (zip deleted, row persists for admin surfacing)
            │                                              │
            ▼                                              │
        MIGRATING ──── failure ───▶ INSTALL_FAILED (schema dropped if first-install per ICD-0.4.3 OQ-C)
            │                                              │
            ▼                                              │
        REGISTERING ── failure ───▶ INSTALL_FAILED (transaction rolled back; schema dropped if first-install)
            │                                              │
            ▼                                              │
        EXTRACTING ─── failure ───▶ INSTALL_FAILED (partial asset dir unlinked)
            │                                              │
            ▼                                              │
        ACTIVATING ─── failure ───▶ INSTALL_FAILED (routes unregistered; handler callables detached)
            │                                              │
            ▼                                              │
         ACTIVE ◀────────────────────────────────────────┘
```

**Every state transition writes `plinth.packages.state`** before calling the next stage. A crash at any state is recoverable from `plinth.packages` + on-disk evidence (§Crash Recovery below).

### UPLOADING

Entry: admin `POST /api/packages` with `multipart/form-data`; the request is authenticated (session or PAT) and RBAC-gated on `packages.install` by `RbacFilter`. Bundled-shell first-boot entry: `install_package(shell_blob, BUNDLED, UserContext::kernel())`.

Stage body:

1. Stream the upload to `{staging_dir}/upload-{uuid}.zip`. Enforce `max_package_size_mb` during streaming — terminate with 413 if exceeded.
2. Validate magic-number (first 4 bytes `PK\x03\x04`). Non-zip → 400, zip deleted, nothing persisted.
3. Unzip to `{staging_dir}/package-{uuid}/`. Use `libzip` (new transitive dep — see §CI Wiring). Path-traversal rejection inline: any archive entry whose normalized path escapes the staging root → 400, staging dir unlinked.
4. Read `manifest.json` to learn the package name (minimal parse — full parse happens at VALIDATING). If the file is missing or the `name` field fails the 0.4.1 regex, 400 with "cannot determine package name from manifest.json".
5. Acquire the package-name advisory lock. If another install is in flight for the same name → 409 Conflict with "another install is in progress for {name}". Lock is held until the lifecycle exits (success or failure).
6. `SELECT 1 FROM plinth.packages WHERE name = ? AND state NOT IN ('UNINSTALLING')` — any row → 409 Conflict "{name} is already installed (0.4.4 does not support upgrade; use 0.4.5's swap path when it ships)". DESIGN §0.4.5 atomic-swap is explicitly out of 0.4.4 scope.
7. INSERT `plinth.packages` row: `state = 'UPLOADING'`, `name` / `version` extracted from the minimal manifest, `provenance`, `manifest_json` populated with the minimal manifest (full manifest replaces this at REGISTERING), `manifest_checksum` computed over the canonical manifest file set.
8. Transition to VALIDATING.

No zip is kept in `{staging_dir}` beyond the lifecycle; success or failure, staging is unlinked at exit (§Cleanup).

### VALIDATING

Stage body:

1. Invoke `plinth::packages::validate(package_root, ValidationConfig{.cross_file = true, .against_running_kernel = true})` — the 0.4.0 + 0.4.2 full pass, now with runtime-state checks live against the in-process PG. The installer supplies a direct PG connection (not via the HTTP `/api/packages/validate` path the CLI uses) so RT1/RT2/RT3 run in-process without a loopback HTTP call.
2. Layer 1 GlassWorm scan is transitive (runs inside `validate()`) per ICD-0.4.1 §4.1.
3. If `report.disposition == 1` (any error): transition to INSTALL_FAILED. `plinth.packages.last_install_report` populated with the full `ValidationReport` JSON. HTTP response: 400 with the report.
4. If `report.disposition ∈ {0, 2}`: transition to MIGRATING. Warnings flow through to the admin response but do not block.

### MIGRATING

Stage body:

1. `plinth::packages::run_migrations(name, package_root, admin_conn)` — ICD-0.4.3's library entry. The installer opens a dedicated admin libpq connection (per ICD-0.4.3 OQ-A resolved to "(a) explicit per-install admin connection").
2. On success: `MigrationReport` persisted to `plinth.packages.last_install_report` (merged with the ValidationReport). Transition to REGISTERING.
3. On failure: transition to INSTALL_FAILED. If this is a first install (no prior `plinth.packages` row had entered ACTIVE state for this name — tracked via the same query pattern), the installer additionally calls a companion `drop_schema_and_migrations(name, admin_conn)` (new in 0.4.4, mirrors ICD-0.4.3 OQ-C resolution: library stays monotonic, orchestrator owns rollback). The companion drops `ext_{name}` CASCADE, drops `ext_{name}_role`, and deletes matching rows from `plinth.migrations`. For upgrade paths (0.4.5), the companion is NOT called — stickiness is preserved. 0.4.4 is first-install-only so the companion always runs on MIGRATING failure.

### REGISTERING

Stage body, all inside a single PG transaction on the same admin connection used for migrations (nested transaction via a savepoint is avoided — the transaction is opened by REGISTERING and covers only its own work):

1. `plinth.packages` row UPDATE: `state = 'REGISTERING'`, `manifest_json` upgraded to the full parsed manifest, `entry_point`, `frontend_mount` (if present), `manifest_checksum` finalized.
2. For each capability in `capabilities.json.provides`: `plinth::capabilities::register_capability(signature, rbac_rule, description, provider_type = EXTENSION, extension_name = name, scope)` — the ICD-0.2.0 entry. This is the integration point with the capability registry: the cache gets populated via the existing LISTEN/NOTIFY path from ICD-0.2.3 (no direct cache write). Bulk failure → transaction rollback.
3. For each rule in `rbac.json.rules`: `plinth::rbac::upsert_extension_rule(rule, namespace, description, extension_name = name)` — the minimal shim. ICD-0.1.4's existing `rbac_rules` CRUD is reused; no test-contract handling in 0.4.4 (0.4.6's scope).
4. For each entry in `panels.json`: `plinth::packages::register_panel(package_id, panel_id, panel_type, slot_type, declaration)` — NEW in 0.4.4. Inserts into `plinth.panels`.
5. Final UPDATE `plinth.packages.state = 'EXTRACTING'`. COMMIT.

On any step's failure, ROLLBACK. Transition to INSTALL_FAILED. On first-install, `drop_schema_and_migrations` runs (same as MIGRATING failure).

### EXTRACTING

Stage body (filesystem-only; no DB writes):

1. Canonical layout: `{data_dir}/extensions/{name}/{version}/` created with mode 0755. Subdirectories `client/`, `server/`, `migrations/`, plus root-level manifest files, copied from the staging unpack tree. Files are `fsync`-ed (per-file, via `std::filesystem::copy` + explicit `fd.sync` follow-up — `libc++`'s `copy` does not sync by default).
2. `{data_dir}/extensions/{name}/active` symlink created (or updated, but 0.4.4 is first-install only) pointing at `{version}/`. Atomic via `symlink` + `rename`.
3. Staging dir `{staging_dir}/package-{uuid}/` removed.
4. Transition to ACTIVATING.

On any filesystem error: transition to INSTALL_FAILED. Cleanup: best-effort unlink of `{data_dir}/extensions/{name}/{version}/`. If the symlink was already created, it is unlinked. The `plinth.packages` row remains for admin surfacing. `drop_schema_and_migrations` runs for first-install.

### ACTIVATING

Stage body:

1. Register the Drogon asset route: `GET /ext/{name}/{version}/{path:.*}` → `asset_server::handle_request(name, version, path)`. Route registration uses Drogon's `app().registerHandler()` with a per-route registration token stored in memory (needed for deregistration by 0.4.5). Since `{version}` is embedded in the route at registration, each version has its own handler — no dynamic `active` symlink lookup on the hot path; the route references `{data_dir}/extensions/{name}/{version}/client/` directly.
2. If `manifest.frontend.mount` is set: register the `GET {mount}/{path:.*}` SPA-fallback route. The frontend-mount handler is distinct from the asset-route handler — see `architecture/06-frontend.md` for the SPA-serving contract (the 0.4.4 handler is minimal; full frontend serving may grow in 0.6.x).
3. Capability handlers become callable: this is automatic because REGISTERING already wrote to `plinth.capabilities` and the LISTEN/NOTIFY cache-invalidation from ICD-0.2.3 propagates the new entries to every node. No explicit activation step.
4. UPDATE `plinth.packages.state = 'ACTIVE'`. (Separate transaction from REGISTERING — REGISTERING already committed; this is a short UPDATE on the existing admin connection.)
5. Emit audit event `packages.installed{name, version, provenance, installed_by_user_id}`.
6. Return the `plinth.packages` row to the HTTP caller (HTTP 201 + JSON body).

On failure: deregister any routes successfully added; UPDATE `plinth.packages.state = 'INSTALL_FAILED'`; emit `packages.install_failed` audit. Cleanup: the schema stays (already committed), migrations stay, `plinth.packages` row stays flagged failed. A first-install ACTIVATING failure is the one case where `drop_schema_and_migrations` does NOT run — cleanup is purely route-unregistration. Admin uses the 0.4.5 uninstall path to clean up when available.

### ACTIVE

Terminal success state in 0.4.4. Packages stay ACTIVE indefinitely until a future 0.4.5 transition moves them to DISABLED / UNINSTALLING.

### INSTALL_FAILED

Terminal failure state in 0.4.4. The row's `last_install_report` holds the full validation + migration error detail for admin inspection. No automatic retry; the admin re-uploads to trigger a fresh install (the name must be released first via 0.4.5 uninstall once that ships — in the 0.4.4-only window, INSTALL_FAILED rows require manual cleanup or await 0.4.5).

---

## HTTP Surface

```
POST  /api/packages             — upload a zip (admin; multipart/form-data)
GET   /api/packages             — list installed packages (admin; paginated)
GET   /api/packages/{id}        — single package record (admin)
GET   /ext/{name}/{version}/*   — static asset (public; per §2.3 auth & CORS)
```

### POST /api/packages

Auth: session cookie OR PAT. RBAC: `packages.install` required. Anonymous → 401; authenticated-without-rule → 403.

Request:
- `Content-Type: multipart/form-data`
- Field `package`: the zip bytes
- Optional query parameter `?dry_run=1`: runs UPLOADING + VALIDATING and returns the `ValidationReport`; does not proceed to MIGRATING. No `plinth.packages` row persists on dry run.

Response on success (201):
```json
{
  "id": "3c7e8b1e-...",
  "name": "notes",
  "version": "1.2.3",
  "state": "ACTIVE",
  "provenance": "user",
  "installed_at": "2026-04-19T18:22:00Z",
  "frontend_mount": null,
  "warnings": [ /* any non-blocking warnings from validation */ ]
}
```

Response on dry-run success (200) — `?dry_run=1` only:
```json
{
  "state": "VALIDATING",
  "name": "notes",
  "version": "1.2.3",
  "frontend_mount": null,
  "validation_report": { /* full ValidationReport: disposition + diagnostics */ }
}
```
No `id` field (no row persists). Status is **200**, not 201 (no resource was created). Dry-run inherits the UPLOADING collision branches: a same-name dry-run while a real install is in flight or already committed returns the same 409 + `kind` (`advisory-lock-held` / `name-already-installed` / `upgrade-version-not-newer`) the regular path produces.

Response on INSTALL_FAILED (400, 422, or 500 depending on failure stage):
```json
{
  "id": "3c7e8b1e-...",
  "name": "notes",
  "version": "1.2.3",
  "state": "INSTALL_FAILED",
  "last_install_report": { /* full ValidationReport + MigrationFailure if applicable */ },
  "failed_at_stage": "MIGRATING"
}
```

HTTP status code mapping:
| Failure stage | Status |
|---|---|
| UPLOADING — bad zip, oversized, magic-number fail | 400 / 413 |
| UPLOADING — manifest missing or name unparseable | 400 |
| UPLOADING — name already installed | 409 |
| UPLOADING — advisory-lock conflict | 409 |
| VALIDATING — validation errors | 422 |
| MIGRATING — migration failure | 422 |
| REGISTERING — transaction rollback | 500 |
| EXTRACTING — filesystem error | 500 |
| ACTIVATING — route registration failure | 500 |

### GET /api/packages

Auth: `packages.read` rule. Returns paginated list of `plinth.packages` rows (default limit 50, `?limit=`, `?offset=`). Rows include `id`, `name`, `version`, `state`, `installed_at`, `frontend_mount`. Excludes failed + uninstalling states unless `?include_failed=1`.

### GET /api/packages/{id}

Auth: `packages.read` rule. Returns a single full `plinth.packages` row. 404 if not found.

### GET /ext/{name}/{version}/{path:.*}

Auth: **public by default** per DESIGN — extension assets are served like any other static content; no cookie required. The asset contract:

- Resolution: `{data_dir}/extensions/{name}/{version}/client/{path}`.
- Route lookup: exact-match against the `(name, version)` pair registered at ACTIVATING. Unknown `(name, version)` → 404. 0.4.4 does not serve old versions (no retention window yet).
- Path normalization: reject `..` components inline; `realpath`-resolve the final path and reject if it escapes `{data_dir}/extensions/{name}/{version}/client/`. The validator's 0.4.0 symlink rejection made the on-disk tree symlink-free, so `realpath` returns a path under the prefix modulo an attacker-supplied request; the post-resolution escape check is belt-and-braces.
- MIME type: static table keyed on lowercased file extension. Initial table:
  ```
  .js   → application/javascript
  .mjs  → application/javascript
  .css  → text/css
  .html → text/html; charset=utf-8
  .json → application/json
  .svg  → image/svg+xml
  .png  → image/png
  .jpg  → image/jpeg
  .jpeg → image/jpeg
  .webp → image/webp
  .woff2 → font/woff2
  .gif  → image/gif
  (unknown → application/octet-stream)
  ```
- Cache headers: `Cache-Control: public, max-age=31536000, immutable`. Version-scoped URLs make this safe — every byte change ships at a new `{version}`.
- `ETag: W/"{manifest_checksum}"`. Reuses the same SHA-256 captured at UPLOADING.
- `Content-Type`: from the MIME table. `application/octet-stream` for unknown extensions.
- `Content-Security-Policy`: not set by this handler. CSP composition is the frontend's concern per `architecture/06-frontend.md §3` and lands in 0.6.x. 0.4.4 leaves the header unset.

404 conditions:
- Unknown `{name}` or unknown `{version}` for the given name.
- Path escapes the extension's `client/` root.
- Requested path is a directory (no directory-index service in 0.4.4).

---

## Data Model

### `plinth.packages` (new)

```sql
CREATE TABLE plinth.packages (
    id                    UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    name                  TEXT        NOT NULL,
    version               TEXT        NOT NULL,
    state                 TEXT        NOT NULL CHECK (state IN (
                                          'UPLOADING', 'VALIDATING', 'MIGRATING',
                                          'REGISTERING', 'EXTRACTING', 'ACTIVATING',
                                          'ACTIVE', 'ACTIVE_FLAGGED',
                                          'DISABLED', 'INSTALL_FAILED', 'UNINSTALLING'
                                      )),
    provenance            TEXT        NOT NULL CHECK (provenance IN ('bundled', 'user')),
    manifest_json         JSONB       NOT NULL,
    frontend_mount        TEXT,
    entry_point           TEXT        NOT NULL,
    manifest_checksum     TEXT        NOT NULL,
    last_install_report   JSONB,
    installed_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    disabled_at           TIMESTAMPTZ,                              -- populated by 0.4.5
    uninstalling_at       TIMESTAMPTZ,                              -- populated by 0.4.5
    last_rbac_test_run_at   TIMESTAMPTZ,                              -- populated by 0.4.7
    last_rbac_test_result   JSONB,                                    -- populated by 0.4.7
    installed_by_user_id  UUID REFERENCES plinth.users(id),
    UNIQUE (name, version)
);

CREATE UNIQUE INDEX uniq_packages_name_active
    ON plinth.packages(name)
    WHERE state IN ('ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED');

CREATE UNIQUE INDEX uniq_packages_mount_active
    ON plinth.packages(frontend_mount)
    WHERE frontend_mount IS NOT NULL
      AND state IN ('ACTIVE', 'ACTIVE_FLAGGED');

CREATE INDEX idx_packages_state ON plinth.packages(state);
```

Column notes:
- `state` CHECK enumeration includes all 0.4.5 / 0.4.7 states even though 0.4.4 writes only the subset above — keeps the schema stable across the arc (no ALTER TABLE churn at 0.4.5 / 0.4.7).
- `last_install_report` stores the full validation + migration report JSON for admin inspection. Bounded to the 50 MB package size; in practice a few KB.
- `installed_by_user_id` NULL for bundled-shell first-boot (no admin user).
- The `uniq_packages_name_active` partial index supersedes DESIGN §4.1's plain `(name, version)` uniqueness with a tighter rule: at most one installed version at a time, which is 0.4.4's first-install-only posture. 0.4.5 will relax this when the retention window grows.

### `plinth.panels` (new)

```sql
CREATE TABLE plinth.panels (
    package_id      UUID        NOT NULL REFERENCES plinth.packages(id) ON DELETE CASCADE,
    panel_id        TEXT        NOT NULL,
    panel_type      TEXT        NOT NULL CHECK (panel_type IN ('primary', 'float', 'settings', 'tray')),
    slot_type       TEXT        CHECK (slot_type IN ('home')),
    declaration     JSONB       NOT NULL,
    registered_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (package_id, panel_id)
);
```

Schema matches DESIGN §4.3 exactly. CASCADE DELETE means 0.4.5's uninstall removes panel rows without bespoke handling. The shell (0.6.x) queries this table via a future kernel capability — `plinth.panels` is kernel-private in 0.4.4.

### Existing tables unchanged

`plinth.migrations` (0.4.3 writer), `plinth.capabilities` (0.2.0 writer), `plinth.rbac_rules` (0.1.4 writer) — all consumed. No ALTERs.

---

## Library Surface

```cpp
// src/kernel/packages/install_lifecycle.hpp — NEW in 0.4.4
#pragma once
#include <expected>
#include <filesystem>
#include <span>
#include <string>

#include "manifest.hpp"  // 0.4.1
#include "validator.hpp"  // 0.4.0/0.4.2

namespace plinth::packages {

enum class Provenance {
    USER,      // admin-uploaded via POST /api/packages
    BUNDLED,   // kernel first-boot shell blob
};

enum class InstallStage {
    UPLOADING, VALIDATING, MIGRATING, REGISTERING,
    EXTRACTING, ACTIVATING, ACTIVE, INSTALL_FAILED,
};

struct PackageRecord {
    std::string id;                     // UUID as string
    std::string name;
    std::string version;
    InstallStage state;
    Provenance provenance;
    std::optional<std::string> frontend_mount;
    nlohmann::json manifest_json;
    std::chrono::system_clock::time_point installed_at;
};

struct InstallFailure {
    InstallStage failed_at;
    std::string package_id;             // UUID of the plinth.packages row (empty if UPLOADING failed before INSERT)
    std::string message;
    nlohmann::json report;              // validation report / migration failure / filesystem error detail
};

struct InstallerContext {
    // Admin libpq connection — installer owns it, closes on function exit.
    PGconn* admin_conn;
    // Auth context; UserContext::kernel() for bundled-shell first-boot, else authenticated admin.
    plinth::auth::UserContext caller;
    std::filesystem::path data_dir;     // {data_dir}/extensions/ is the install target
    std::filesystem::path staging_dir;  // scratch space
    std::size_t max_package_size;
};

auto install_package(std::span<const std::byte> zip_blob,
                     Provenance provenance,
                     InstallerContext& ctx)
    -> std::expected<PackageRecord, InstallFailure>;

auto reconcile_in_flight_installs(InstallerContext& ctx) -> void;

auto drop_schema_and_migrations(std::string_view name, PGconn& admin_conn)
    -> std::expected<void, MigrationFailure>;

}  // namespace plinth::packages
```

```cpp
// src/kernel/packages/panels.hpp — NEW in 0.4.4
#pragma once
#include <expected>
#include <string>

namespace plinth::packages {

enum class PanelType { PRIMARY, FLOAT, SETTINGS, TRAY };
enum class SlotType { HOME };  // nullable — std::optional<SlotType> in callers

struct PanelRegistration {
    std::string package_id;
    std::string panel_id;
    PanelType panel_type;
    std::optional<SlotType> slot_type;
    nlohmann::json declaration;
};

auto register_panel(PGconn& conn, const PanelRegistration& reg)
    -> std::expected<void, std::string>;

}  // namespace plinth::packages
```

```cpp
// src/kernel/packages/asset_server.hpp — NEW in 0.4.4
#pragma once
#include <drogon/HttpController.h>

namespace plinth::packages {

auto register_asset_routes(std::string_view name,
                           std::string_view version,
                           const std::filesystem::path& client_root) -> void;

auto unregister_asset_routes(std::string_view name,
                             std::string_view version) -> void;  // used by 0.4.5

}  // namespace plinth::packages
```

### RBAC shim

`plinth::rbac::upsert_extension_rule(rule, namespace, description, extension_name)` — new in 0.4.4. Uses ICD-0.1.4's existing `plinth.rbac_rules` INSERT. On 0.4.6, this function gets extended to accept `test_contract JSONB` and to perform rule validation; the signature change is additive.

---

## Error Taxonomy

Surfaced in `InstallFailure.report` as structured JSON. Top-level `kind` discriminates stages:

| `kind` | Stage | Condition |
|---|---|---|
| `upload-too-large` | UPLOADING | Streamed bytes exceeded `max_package_size`. |
| `not-a-zip` | UPLOADING | Magic-number check failed. |
| `path-traversal` | UPLOADING | A zip entry's normalized path escaped the staging root. |
| `bad-manifest-name` | UPLOADING | Minimal parse of `manifest.json` could not extract a valid `name`. |
| `name-already-installed` | UPLOADING | Another row for the same name exists in a non-UNINSTALLING state. |
| `advisory-lock-held` | UPLOADING | Another in-flight install holds the name's advisory lock. |
| `validation-errors` | VALIDATING | Full `ValidationReport` attached. |
| `migration-failed` | MIGRATING | `MigrationFailure` attached (ICD-0.4.3 surface). |
| `registration-failed` | REGISTERING | PG error + the specific sub-stage (package row, capability, rule, panel). |
| `extraction-failed` | EXTRACTING | `std::filesystem::filesystem_error` with code + path. |
| `activation-failed` | ACTIVATING | Drogon route-registration error. |

All errors are logged to `plinth.audit_log` as `packages.install_failed{name, version, stage, kind}`.

---

## Security Constraints

1. **Zip extraction is closed-domain.** No entry in the archive may resolve outside the staging root. Check happens on the normalized path *before* any filesystem write. Unicode-normalization of archive entry names is off (raw bytes) — the 0.4.1 GlassWorm scan does not apply to archive entry names (Layer 1 targets `.js` / `.json` content, not filesystem metadata).

2. **`max_package_size` is enforced at the streaming boundary, not post-unzip.** A zip-bomb with 50 MB compressed expanding to 5 GB would exhaust disk before the post-unzip size check. Counter: sum `uncompressed_size` from each archive entry's header before any extraction; reject if total > `max_package_size * 2` (2× factor accommodates legitimate high-ratio compression like large JSON).

3. **Staging directory is kernel-private.** `{staging_dir}` permissions 0700, owned by the kernel's Unix user. No extension ever sees its contents. Cleaned up unconditionally on lifecycle exit.

4. **`POST /api/packages` is admin-gated.** `packages.install` rule seeded only on the built-in admin group. A compromised extension cannot install another extension — extensions don't have HTTP-endpoint registration in 0.4.4 at all.

5. **Bundled-shell first-boot uses `UserContext::kernel()`, not an admin user.** No audit-log record of "the kernel installed itself" with a human user ID — `installed_by_user_id` is NULL. `plinth.audit_log` entry has `user_id = NULL`, `node_id` set.

6. **Asset serving is public.** `GET /ext/{name}/{version}/*` requires no auth. This is deliberate — extension assets (JS, CSS, icons) are loaded unauthenticated by the browser before or during login. Anything an extension wants to keep private must live outside `client/` and be fetched via an authenticated capability call.

7. **Path traversal on the asset route is belt-and-braces.** Even though 0.4.0's R2 symlink rejection made the on-disk tree safe, the asset handler normalizes the request path before resolution and rejects any `..` component. Defense against future symlinks appearing through an unrelated bug path.

8. **Deleting `plinth.packages` rows mid-flight is not supported.** A `DELETE FROM plinth.packages WHERE id = ?` while state is `MIGRATING` would leave the schema and on-disk files orphaned. Admin DELETE access is not exposed by 0.4.4 (0.4.5's uninstall path is the only supported deletion). PG-level DELETE access is reserved to the kernel admin role.

9. **`packages.install` grant model matches bootstrap convention.** The rule is added to `plinth.rbac_rules` by `bootstrap_groups` (or its ICD-0.1.4-era equivalent) alongside `kernel.admin`. The admin group gets the grant at seed time. No runtime self-grant path.

---

## Test Cases

Fixtures under `tests/fixtures/install_lifecycle/` — separate tree from the 0.4.0 / 0.4.2 validator fixtures because install-lifecycle fixtures are *valid* packages by construction (the failure modes are all about runtime state or mid-install conditions, not about malformed inputs). All tests gated `PLINTH_KERNEL_TESTS=ON`.

| # | Case | Expected |
|---|---|---|
| I.01 | `valid-install/` — clean install from zip | ACTIVE; asset route responds; capability registry has entries; `plinth.packages`/`panels`/`rbac_rules`/`migrations` rows all present |
| I.02 | `valid-install-no-panels/` — no `panels.json` | ACTIVE; `plinth.panels` empty for this package |
| I.03 | `valid-install-frontend/` — has `manifest.frontend.mount` | ACTIVE; SPA-fallback route registered; `plinth.packages.frontend_mount` set |
| I.04 | Upload same name twice without uninstall | Second: 409 Conflict; first row remains ACTIVE |
| I.05 | Upload oversized zip (61 MB with 50 MB limit) | 413; nothing persisted |
| I.06 | Upload non-zip (PDF with `.zip` extension) | 400 not-a-zip; nothing persisted |
| I.07 | Zip with path-traversal entry (`../evil.txt`) | 400 path-traversal; nothing persisted |
| I.08 | Manifest missing | 400 bad-manifest-name |
| I.09 | Validation error (validator CF1 fails) | INSTALL_FAILED; 422; schema cleaned |
| I.10 | Migration syntax error | INSTALL_FAILED at MIGRATING; schema dropped; `plinth.migrations` empty for extension |
| I.11 | Kernel killed mid-MIGRATING (SIGKILL) then restarted | `reconcile_in_flight_installs` marks row INSTALL_FAILED; schema may be partial — admin sees clear flag |
| I.12 | Kernel killed mid-ACTIVATING then restarted | `reconcile_in_flight_installs` advances to ACTIVE if files present and routes re-registerable; else INSTALL_FAILED |
| I.13 | `GET /ext/notes/1.2.3/main.js` on ACTIVE package | 200, correct `Content-Type`, immutable cache header |
| I.14 | `GET /ext/notes/1.2.3/..%2fsecret.txt` (URL-encoded traversal) | 404 (not 200, not 500) |
| I.15 | `GET /ext/unknown/1.0.0/foo.js` | 404 |
| I.16 | Bundled-shell first boot — no frontend installed at kernel start | Shell installs, state ACTIVE, `provenance = bundled`, `installed_by_user_id = NULL` |
| I.17 | Bundled-shell first boot — frontend already installed | First-boot hook is a no-op; no duplicate row |
| I.18 | Concurrent POSTs for the same package name | First succeeds; second receives 409 advisory-lock-held while first is in flight |
| I.19 | Dry-run (`?dry_run=1`) with valid package | 200 with `ValidationReport`; no row in `plinth.packages`; no schema created _(closed in 0.6.0.N session 7, 2026-04-28; three sub-cases I.19a/b/c at `tests/kernel/packages/install_lifecycle_http_test.cpp`)_ |
| I.20 | Admin without `packages.install` rule | 403 |

Test count target: **20 fixtures + Catch2 driver case per row**. I.11 / I.12 require a custom child-process harness (fork, exec, SIGKILL, exec-parent-resume) that extends the `PLINTH_KERNEL_TESTS` fixture layer.

**Unit-level coverage** (non-PG) in a separate `install_lifecycle_unit_test.cpp`:
- Zip-entry path-traversal rejection logic (without touching the filesystem).
- MIME table lookup.
- State-enum string conversions round-trip.
- JSON schema for `InstallFailure.report`.

---

## CI Wiring

- `migrations/schema.sql` — `plinth.packages` + `plinth.panels` table definitions added under the existing "Packages — Added in 0.4.0" section header (which becomes truthful at this milestone).
- `src/kernel/packages/install_lifecycle.{hpp,cpp}` — **new**. Main orchestrator.
- `src/kernel/packages/panels.{hpp,cpp}` — **new**. Panel CRUD.
- `src/kernel/packages/asset_server.{hpp,cpp}` — **new**. Drogon asset handler + route registration/deregistration.
- `src/kernel/packages/handlers.{hpp,cpp}` — **new**. `POST /api/packages`, `GET /api/packages`, `GET /api/packages/{id}` Drogon controllers.
- `src/kernel/packages/shell_blob.{hpp,cpp}` — **new**. Extern-symbol access to the linker-embedded shell zip.
- `src/kernel/rbac/rule_registrar.cpp` (existing from ICD-0.1.4) — gains `upsert_extension_rule()` shim (additive).
- `src/kernel/main.cpp` — hooks added: after `bootstrap_schema`, call `reconcile_in_flight_installs()`, then the shell first-boot check, then `register_asset_routes()` for each ACTIVE package. Route re-registration on kernel restart iterates `SELECT id, name, version FROM plinth.packages WHERE state IN ('ACTIVE', 'ACTIVE_FLAGGED')`.
- `bootstrap_groups` (from ICD-0.1.4) — `packages.install` + `packages.read` rules seeded.
- `CMakeLists.txt` — new `shell_blob` target that packs the shell source tree into an object file using `ld -r -b binary` (gcc/clang convention) or a custom `objcopy` rule; linked into the `plinth` binary. **libzip dependency added** via FetchContent or system-package detection (vcpkg / apt). Build-config choice at implementation time; FetchContent keeps the dependency reproducible.
- `tests/fixtures/install_lifecycle/` — 20 new fixture packages/directories.
- `tests/kernel/packages/install_lifecycle_test.cpp` — **new**. PG-gated.
- `tests/kernel/packages/install_lifecycle_unit_test.cpp` — **new**. No PG gate.
- `tests/kernel/packages/asset_server_test.cpp` — **new**. Covers I.13–I.15. Uses Drogon's test harness (no PG required, purely HTTP-route-level).
- `run-clang-tidy-20` zero findings on new TUs.
- CI: both PG-gated and non-PG test layers green. Integration tests under `PLINTH_KERNEL_TESTS=ON` add ~20 seconds to the existing test block; crash-recovery cases add ~30 seconds (fork/exec/kill overhead).

---

## Entry / Exit

**Entry:** ICD-0.4.0 + ICD-0.4.1 + ICD-0.4.2 + ICD-0.4.3 merged; capability registry (ICD-0.2.0 through ICD-0.2.4) operational; QuickJS runtime pool (ICD-0.3.1) operational; RBAC enforcement (ICD-0.1.5) operational; audit log (ICD-0.1.7) operational. The kernel's admin PG credentials are available via existing bootstrap config.

**Exit:** All 20 I.* cases green under `PLINTH_KERNEL_TESTS=ON`; non-PG unit cases green under default `ctest`; `run-clang-tidy-20` clean on new TUs; `POST /api/packages` manually verified with a local kernel; `GET /ext/{name}/{version}/*` manually verified against an installed package's JS bundle in a browser (cache header observable in DevTools); bundled-shell first-boot verified by wiping `plinth.packages` and restarting the kernel; CHANGELOG entry describes the state machine + new schema tables + new HTTP surface + the shell first-boot mechanism; ROADMAP `0.4.4` line removed; DEFERRED.md amended if the `libzip` / FetchContent decision or the `shell_blob` linker-pack approach surfaces a new deferral.

---

## Open Questions (resolve during implementation)

1. **Upgrade-vs-reject decision** (inherited from ICD-0.4.2 OQ #6). 0.4.4 is first-install-only, which resolves this by truncation: any collision = reject with 409. The 0.4.5 disposition (version-comparison; newer → atomic-swap upgrade path; same-or-older → reject) is deferred cleanly. Proposed: document "first-install-only" as an explicit 0.4.4 invariant in the PR and reiterate in the 0.4.5 ICD authoring session. Architect confirmation requested at PR time.

2. **`libzip` vs. home-rolled zip parse.** Options: (a) FetchContent libzip (mature, handles edge cases, ~200 KB binary cost); (b) minizip-ng (smaller, slightly less ergonomic); (c) home-rolled minimal parser (archive-format spec is ~50 pages; our needs are limited to central-directory read + per-file inflate, which is ~600 LOC). Proposed: (a) libzip. Rationale: zip-parsing edge cases are a known historical source of CVEs (zip-slip, symlink traversal, zero-size-with-data records), and libzip is audited. Cost is ~200 KB binary size — the kernel is already ~40 MB. Architect preference at PR time.

3. **Shell-blob packing mechanism.** Options: (a) `ld -r -b binary` produces an object with symbols `_binary_shell_zip_start/_end/_size`. Widely supported, gcc/clang-compatible, but produces architecture-specific objects. (b) `xxd -i` / CMake `configure_file` generates a C array header. Architecture-independent, but the zip goes into rodata as a massive byte array — slower link, larger preprocessed source. (c) Deferred to 0.6a (shell as a separately-built extension, no bundling). Proposed: (a) for 0.4.4 since first-boot must work out of the box; revisit at 0.6a if bundling becomes a CI-build pain point. Architect preference.

4. **Validation-at-install uses in-process call or HTTP loopback?** ICD-0.4.2 has `--against-running-kernel` wired as an HTTP call from the CLI. The installer has direct access to the PG connection and capability registry in-process; an HTTP loopback would serialize + parse + re-serialize the `ValidationReport` unnecessarily. Proposed: installer calls `plinth::packages::validate()` directly with `ValidationConfig{.against_running_kernel = true, .kernel_url = std::nullopt}` and a supplemental flag `ValidationConfig::in_process_registry = true` (NEW in 0.4.4, additive) that tells the validator to skip the HTTP path and use the in-process kernel surfaces. The CLI path keeps its HTTP contract. Architect decision at PR time; this affects ICD-0.4.2's `ValidationConfig` surface additively.

5. **Audit-event granularity.** Per-stage audit events vs. one `packages.installed` on success + one `packages.install_failed` on failure. Proposed: one-per-stage transition (`packages.install_validating`, `packages.install_migrating`, ..., `packages.installed` on ACTIVE, or `packages.install_failed` on INSTALL_FAILED). Rationale: the audit log is the canonical forensic record; per-stage granularity helps reproduce mid-install failures. Cost: ~7 more audit rows per install. 0.4.4 installs are rare; the cost is negligible. Alternative: only terminal events (one row per install). Architect preference.

6. **Crash-recovery disposition on mid-MIGRATING.** `reconcile_in_flight_installs` sees `plinth.packages.state = 'MIGRATING'` on startup but cannot cheaply know which migration was mid-flight. Proposed: always mark as INSTALL_FAILED; the admin re-uploads to trigger a fresh install. The alternative (try to resume) is risky — migrations may have had side effects outside the transaction (e.g. a `CREATE TABLE` that committed before a subsequent `INSERT` in the same migration file failed, which is a violation of the per-migration transaction but possible if the migration author used explicit `COMMIT`). Architect acceptance at PR.

---

## Appendix: Example Install Timeline

Concrete walk-through for a hypothetical Notes extension, anchoring the state machine:

```
t=0.000s  Admin: curl -X POST https://plinth.example.com/api/packages \
                    -H "Authorization: Bearer $PAT" \
                    -F "package=@notes-1.2.3.zip"
t=0.020s  RbacFilter: authenticated admin; packages.install rule present. Forward to handler.
t=0.021s  UPLOADING: stream zip (4.2 MB) to /var/lib/plinth/staging/upload-abc.zip
t=0.085s  Magic number OK. libzip opens archive. Zip entries checked for path traversal — pass.
t=0.110s  libzip extracts to /var/lib/plinth/staging/package-abc/
t=0.115s  Minimal manifest parse: name="notes", version="1.2.3". Advisory lock on plinth.packages.notes acquired.
t=0.118s  SELECT 1 FROM plinth.packages WHERE name='notes' AND state NOT IN ('UNINSTALLING') → empty.
t=0.120s  INSERT plinth.packages state='UPLOADING' → id=3c7e8b1e-...
t=0.121s  Transition: state='VALIDATING'

t=0.122s  VALIDATING: plinth::packages::validate(package_root, {cross_file=true, against_running_kernel=true, in_process_registry=true})
             Layer 1 GlassWorm scan (0.4.1) → clean
             R1..R6 (0.4.0) → pass
             CF1..CF7, CFW1..CFW4 (0.4.2) → pass
             RT1 (name collision) → miss (no prior row)
             RT2 (mount collision) → N/A (no frontend.mount)
             RT3 (requires resolution) → all satisfied
t=0.180s  ValidationReport{disposition=0, messages=[]}. Transition: state='MIGRATING'.

t=0.181s  MIGRATING: plinth::packages::run_migrations("notes", package_root, admin_conn)
             Creates ext_notes, ext_notes_role, GRANTs (0.4.3 schema/role block)
             Applies 001_create_notes.sql, 002_add_tags.sql
             Two rows inserted into plinth.migrations
t=0.320s  MigrationReport{applied: [001, 002], skipped: [], warnings: []}. Transition: state='REGISTERING'.

t=0.321s  REGISTERING: BEGIN on admin_conn
             UPDATE plinth.packages SET state='REGISTERING', manifest_json=..., entry_point=..., manifest_checksum=... WHERE id=...
             register_capability("notes:1:edit(string)->result", "notes.edit", ..., EXTENSION, "notes", INSTANCE)
             register_capability("notes:1:render(string)->result", "notes.render", ..., EXTENSION, "notes", INSTANCE)
             upsert_extension_rule("notes.edit", "notes", "Edit notes", "notes")
             upsert_extension_rule("notes.render", "notes", "Render notes", "notes")
             register_panel(package_id, "editor", PRIMARY, nullopt, {declaration...})
             UPDATE plinth.packages SET state='EXTRACTING' WHERE id=...
             COMMIT
             LISTEN/NOTIFY fires — capability cache on every node invalidated + reloaded (0.2.3 surface)

t=0.450s  EXTRACTING: copy /var/lib/plinth/staging/package-abc/* to /var/lib/plinth/extensions/notes/1.2.3/
             fsync each file
             symlink /var/lib/plinth/extensions/notes/active -> 1.2.3
             unlink /var/lib/plinth/staging/package-abc/
t=0.520s  Transition: state='ACTIVATING'.

t=0.521s  ACTIVATING: register_asset_routes("notes", "1.2.3", /var/lib/plinth/extensions/notes/1.2.3/client)
             Drogon: GET /ext/notes/1.2.3/* handler live
             (No frontend.mount in this example; SPA-fallback skipped.)
t=0.540s  UPDATE plinth.packages SET state='ACTIVE' WHERE id=...
t=0.541s  Audit: packages.installed{name=notes, version=1.2.3, provenance=user, installed_by_user_id=...}
t=0.542s  Release advisory lock.

t=0.543s  HTTP 201 to admin:
             {
               "id": "3c7e8b1e-...",
               "name": "notes",
               "version": "1.2.3",
               "state": "ACTIVE",
               "provenance": "user",
               "frontend_mount": null,
               "installed_at": "2026-04-19T18:22:00.542Z",
               "warnings": []
             }

t=0.600s  User browser: GET /ext/notes/1.2.3/main.js
             asset_server: realpath-normalize → /var/lib/plinth/extensions/notes/1.2.3/client/main.js → reads + returns
             Response: 200, Content-Type: application/javascript, Cache-Control: public, max-age=31536000, immutable, ETag: W/"a1b2c3..."
t=0.650s  First capability call: notes:1:edit("hello")
             Through RbacFilter → denied until user is in a group granted notes.edit
             Admin grants notes.edit to 'authors' group; user is in 'authors'
             Second call succeeds.
```

**Bundled-shell first-boot timeline:**

```
Kernel start (no frontend installed)
  ↓
bootstrap_schema() — schema.sql applied
  ↓
reconcile_in_flight_installs() — zero rows to reconcile on fresh DB
  ↓
SELECT 1 FROM plinth.packages WHERE frontend_mount IS NOT NULL AND state IN ('ACTIVE', 'ACTIVE_FLAGGED') → empty
  ↓
get_embedded_shell_package() — returns span<const byte> over the linker-embedded zip
  ↓
install_package(shell_blob, BUNDLED, UserContext::kernel(), ...)
  ↓
UPLOADING → VALIDATING → MIGRATING → REGISTERING → EXTRACTING → ACTIVATING → ACTIVE
  ↓
plinth.packages row inserted with provenance='bundled', installed_by_user_id=NULL
  ↓
Shell's /ext/shell/1.0.0/* route live
  ↓
app().run() — HTTP server accepts traffic
```

A second kernel start with the shell already installed short-circuits at the `SELECT 1 FROM plinth.packages WHERE frontend_mount IS NOT NULL` check — no duplicate install.
