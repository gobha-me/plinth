# DESIGN: Package System (0.4.x)

**Status:** Draft — v1 for architect review
**Scale:** 2 — Multi-version arc (0.4.0 through 0.4.7)
**Traces to:** architecture/01-identity.md §2 (Groups/RBAC), architecture/02-capabilities.md §1 (Capability Registry), architecture/03-data.md §1 (Database), architecture/05-extensions.md §2 (Reserved URL Prefixes), architecture/05-extensions.md §1 (Package Structure), architecture/05-extensions.md §3 (QuickJS Runtime), architecture/06-frontend.md (Frontend Architecture)
**Depends on:**
- DESIGN-rbac-philosophy.md
- DESIGN-capability-registry.md
- DESIGN-shell-v06x.md
- ICD-0.1.4-groups-rbac (RBAC rule storage contract)
- ICD-0.1.5-rbac-enforcement (enforcement middleware, which 0.4.7 integration tests run through)
- ICD-0.2.0 through ICD-0.2.4 (capability registry — 0.4.4 registers capabilities into it)

**Informs:**
- DESIGN-sharing-v011x.md — must reserve `shareable[]` manifest slot before 0.4 freezes
- DESIGN-shell-v06x.md §1 — shell's first-boot install mechanism depends on 0.4.4
- Every future extension design doc — manifest schema is the stable contract

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Decision date:** 2026-04-16

---

## 1. Decision

The package system is the mechanism by which Plinth acquires capabilities beyond the kernel. Packages are validated, installed, activated, upgraded, disabled, and uninstalled through a kernel-owned lifecycle. Extensions declare their contract through split manifest files; the kernel validates the contract statically at install time and dynamically at runtime.

The 0.4.x arc delivers the complete package lifecycle end-to-end. At the close of 0.4.7, an admin can upload a package, the kernel validates it, creates its PG schema, runs its migrations, registers its RBAC rules and capabilities, serves its client assets at `/ext/{name}/{version}/*`, and runs the extension's capability handlers through the QuickJS bridge. The admin can then disable, re-enable, upgrade, or uninstall the package. The shell (DESIGN-shell-v06x.md) is installed through this exact same lifecycle on first boot.

Manifest schemas defined in this arc are the permanent contract. Extensions built against the 0.4 schemas must continue to install on any 1.x+ kernel without migration.

---

## 2. Full Arc Overview — What 0.4.7 Yields

At the close of 0.4.7, the following must be true:

- **A packaged extension can be installed.** Admin uploads a zipped package; kernel validates, creates schema, runs migrations, registers rules and capabilities, extracts assets, activates handlers, runs Phase-B RBAC tests.
- **Extension assets are served.** `GET /ext/notes/1.2.3/main.js` returns the file with `Cache-Control: public, max-age=31536000, immutable`. Old versions stop serving after the atomic swap drains.
- **The shell installs itself on first boot.** When the kernel starts and no frontend extension is installed, it extracts the bundled shell package blob and runs it through the standard install lifecycle. The shell's manifest is the first test case for every validation rule.
- **Disable, enable, uninstall, upgrade all work.** All state transitions are atomic and leave the system in a consistent state on failure.
- **RBAC rules registered by extensions are enforced.** A capability call from within an extension goes through `RbacFilter` (ICD-0.1.5) using rules registered by the package system. Phase-B integration tests validate the contract.
- **Every failure mode is recoverable.** A failed install rolls back. A corrupt migration halts further migrations. An extension that fails Phase-B is installed-but-flagged, not silently broken.

**What 0.4.7 does not implement, and its relationship to those features:**

Four capabilities are outside the scope of this arc. They fall into two categories, and 0.4's obligations to each differ.

**Deferred but committed — 0.4 must not block:**

- *Hot-reload (0.10.4).* The atomic-swap contract in §8 is the foundation hot-reload builds on. 0.4 establishes that extension versions can be swapped without a kernel restart, handlers can be drained and re-registered, and the filesystem layout supports multiple co-resident versions. Hot-reload in 0.10.4 adds the file-watcher trigger and the developer-mode UX on top of 0.4's atomic swap. **0.4 must not introduce any design that breaks this path.**

**Deferred and uncommitted — 0.4 must not foreclose, but makes no promise:**

- *Public share rendering (`render_share`, 0.11 if picked up).* The `shareable[]` manifest slot is reserved in 0.4.1 specifically to keep this option open. If sharing is never built, the slot sits empty forever and costs nothing. See Appendix A.
- *Remote package registry (`plinth.run` or equivalent).* `ARCHITECTURE.md §8` open question. Packages arrive as uploaded zips in 0.4 and remain so indefinitely unless a future arc changes it. Nothing in 0.4 prevents a network-fetch source from being added as an additive feature.
- *Sidecar packages (0.8.x).* Sidecars may or may not share this manifest format. The 0.8 design session decides. 0.4's manifest is extensible enough that either outcome — shared format with extensions, or separate manifest — is viable.

The practical rule for code sessions working on 0.4.x: **if a design choice would make any of the four above harder to add later, that's a structural question and returns to an architecture session.** If a design choice is simply silent on them, it's fine.

---

## 3. Per-Version Scope

### 0.4.0 — Package Structure Validation (`plinth validate`)

**Scope:** Static validation of a package directory without a running kernel. Implements the `plinth validate ./path/` CLI command left as a TODO in `main.cpp`.

**Reads:** The package directory on disk.
**Writes:** Nothing (stdout/stderr only).

**Validates:**
- Required files exist: `manifest.json`, `capabilities.json`
- Forbidden files/paths absent: symlinks, paths with `..`, absolute paths in the archive, files outside the declared package layout
- JSON files parse
- `server/handlers/*.js` exist for every capability in `capabilities.json` (warning-level)
- `client/panels/*` files referenced by `panels.json` exist (if `panels.json` present)
- Size limits: total uncompressed size ≤ configured max (default 50MB, from `architecture/05-extensions.md §3`)

**Does NOT:**
- Connect to PG
- Execute any extension code
- Resolve capabilities against a running registry (that's cross-file validation, 0.4.2)

**Entry criteria:** Nothing beyond the 0.1.x foundation already merged.
**Exit criteria:** `plinth validate ./test-fixture/` reports pass/fail with line-level error detail; CI fixture set includes one passing and several failing packages covering every validation rule.

---

### 0.4.1 — Manifest Parsing: `manifest.json`, `capabilities.json`

**Scope:** Parsers for the two required manifest files. Typed C++ structs with nlohmann/json deserialization. Shared by `plinth validate` (0.4.0) and the runtime installer (0.4.4).

**New C++ types (under `src/kernel/packages/`):**
- `PackageManifest` — from `manifest.json`
- `CapabilityManifest` — from `capabilities.json`
- `ManifestParseError` — structured error with file, line (where JSON parser provides it), and reason

**Schema for `manifest.json` (this is the permanent contract):**

```json
{
  "name": "notes",
  "version": "1.2.3",
  "description": "Markdown notes with live preview",
  "author": "jeff",
  "license": "MIT",
  "entry_point": "server/main.js",

  "frontend": {
    "mount": "/app",
    "entry": "index.html"
  },

  "runtime": {
    "memory_limit_mb": 64,
    "cpu_time_limit_ms": 5000,
    "max_stack_depth": 1000
  },

  "shareable": []
}
```

**Field semantics:**

| Field | Required | Notes |
|-------|----------|-------|
| `name` | Yes | Must match `^[a-z][a-z0-9-]{1,63}$`. Becomes the PG schema suffix (`ext_{name}`) and the URL segment (`/ext/{name}/*`). Renames are impossible post-install. |
| `version` | Yes | SemVer 2.0.0. Becomes the URL segment (`/ext/{name}/{version}/*`). Two versions of the same package cannot be active simultaneously. |
| `description` | Yes | Human-readable. Displayed in admin UI. |
| `author` | Yes | Free text. No verification in 0.4. |
| `license` | Yes | SPDX identifier (validated against a static list — MIT, Apache-2.0, GPL-3.0, AGPL-3.0, BSD-3-Clause, ISC, Unlicense; any other value produces an install-time warning). |
| `entry_point` | Yes | Path to the backend entry file. Must exist. Used by the QuickJS runtime pool (0.3.x). |
| `frontend` | **No** | Present only for frontend extensions (the shell, or a BYO replacement). See 0.4.2 for cross-file validation and `architecture/06-frontend.md` for semantics. |
| `frontend.mount` | If `frontend` present | Must start with `/`. Must not overlap any reserved prefix (`architecture/05-extensions.md §2`). Must not be `/ext` or any subpath thereof. |
| `frontend.entry` | If `frontend` present | Relative path within `client/`. Must exist. |
| `runtime` | No | Per-extension runtime limit overrides. Kernel clamps to configured maxima (`architecture/05-extensions.md §3`). |
| `shareable` | No (reserved) | **Reserved slot for 0.11.** Must parse as an array; must be empty in 0.4.x. Any non-empty value produces an install-time warning "shareable is reserved for a future version" and is ignored. See Appendix A. |

**Schema for `capabilities.json`:** unchanged from `architecture/02-capabilities.md §1` and DESIGN-capability-registry.md. The parser validates structural correctness (every entry has namespace/version/function, types are strings, etc.). Semantic validation (namespace matches manifest `name`, required capabilities exist) happens in 0.4.2.

**Entry criteria:** 0.4.0 merged.
**Exit criteria:** Both parsers produce typed structs or structured errors for every fixture package; round-trip test (parse → serialize → parse) produces identical output.

---

### 0.4.2 — Cross-File Manifest Validation Pass

**Scope:** Whole-package validation that requires reading all manifest files together. This is the pass described in `architecture/05-extensions.md §1` "Cross-File Manifest Validation."

**Reads:** All five possible manifest files (`manifest.json`, `capabilities.json`, `rbac.json`, `panels.json`, `config.json`) plus the filesystem layout.

**Produces:** `ValidationReport { errors: [], warnings: [] }`. Errors block installation. Warnings are logged and shown to admin.

**Cross-file rules enforced (errors):**

- Every `rbac.json` rule's `namespace` matches a capability in `capabilities.json`, OR is a kernel-level rule prefix (`kernel.*`, `system.*`, etc. — exact list defined in kernel bootstrap).
- Every capability referenced in `rbac.json` test contracts exists in `capabilities.json` with matching version.
- Every panel entry in `panels.json` references a file that exists under `client/panels/`.
- Every capability with a handler declared in `capabilities.json` has a matching `server/handlers/{function}.js` file. (`architecture/05-extensions.md §1` says this is a warning; upgrading to an error here because the alternative is an install that registers a capability whose handler can't be loaded — a crash-at-first-call failure mode.)
- `frontend.mount` does not conflict with any reserved prefix from `architecture/05-extensions.md §2`.
- Package `name` is not a reserved name (`kernel`, `plinth`, `system`, or any other name reserved by bootstrap).
- Capability namespaces in `capabilities.json` match the package `name` (an extension may only provide capabilities in its own namespace, unless explicitly declared as a provider extension — deferred, not in 0.4).

**Cross-file rules enforced (warnings):**

- Every provided capability has at least one RBAC rule in `rbac.json` (`architecture/05-extensions.md §1`).
- `shareable[]` is non-empty (reserved for 0.11).
- `entry_point` references a file that imports from `client/` paths (sign of a mis-structured package).
- Declared `runtime.memory_limit_mb` exceeds the kernel's configured maximum (will be clamped; warning is informational).

**Runtime-state validation (performed only when a kernel is running; `plinth validate` skips these):**

- No other installed package claims the same `name`.
- No other installed package claims the same `frontend.mount` (at most one extension may claim `/`; at most one may claim any non-root prefix).
- Every capability in `requires` (from `capabilities.json`) exists in the registry, OR is provided by this same package, OR is a deferred kernel capability (warning with list of missing).

**CLI integration:** `plinth validate ./path/` runs the full static set (errors + structural warnings). A new flag `--against-running-kernel` runs the runtime-state validations too (requires a reachable kernel via config).

**Entry criteria:** 0.4.1 merged.
**Exit criteria:** Full cross-file validation covers every rule above; fixture set includes one package per rule violation producing the correct error or warning; CI green.

---

### 0.4.3 — Extension PG Schema Creation and Migration Execution

**Scope:** Create `ext_{name}` schema on install; run numbered migrations in `migrations/`; track applied migrations per-extension in `plinth.migrations`.

**Follows `architecture/03-data.md §1`:**
- Schema name: `ext_{name}` where `{name}` is the package name (already validated as a safe identifier in 0.4.1).
- Migration tracking table: `plinth.migrations` with `(extension_name, migration_file, applied_at, checksum)` primary key.
- Transactional: each migration runs in its own transaction. Failure rolls that migration back; no attempt is made to run subsequent migrations.
- Immutable: checksums detect modification of a previously-applied migration and refuse to proceed with an error.

**PG GRANT management:**
- On schema creation, the kernel grants the extension's PG role `USAGE, CREATE` on `ext_{name}` and `USAGE` on `plinth` for read-only access to whitelisted kernel tables (`plinth.users` for user lookup — exact list to be refined alongside kernel stdlib work).
- The extension's runtime PG connection sets `search_path = ext_{name}, plinth` (`architecture/03-data.md §1`).

**Migration file naming:**
- `migrations/NNN_description.sql` where `NNN` is zero-padded numeric (`001_create_notes.sql`, `002_add_tags.sql`).
- Sort order is numeric, not lexicographic. `010_` sorts after `009_`, not after `001_`.
- Gaps permitted but generate a warning (indicates a skipped migration file).

**Failure handling:**
- If a migration fails, the install lifecycle (0.4.4) treats the whole install as failed. The schema is dropped (if this was a new install) or left in its partially-migrated state (if this was an upgrade — admin must resolve).
- **Upgrade migration failures are sticky.** The extension remains at the pre-upgrade version, the migration is in `plinth.migrations` with `applied_at IS NULL` and an error message, and the admin gets a clear "migration X failed, extension remains at version Y" notification.

**Entry criteria:** 0.4.2 merged; PG connection pool initialized (exists from 0.1.x).
**Exit criteria:** Test package with 3-migration history installs and reinstalls cleanly; modifying an applied migration's content causes a reinstall to fail with a checksum error; a migration with a syntax error fails cleanly with PG error detail surfaced.

---

### 0.4.4 — Package Install Lifecycle (End to End)

**Scope:** The full install path from "admin uploads a zip" to "extension is active and handlers are callable." This is the orchestrator; 0.4.0 through 0.4.3 are its components. Also implements **asset extraction and serving** at `/ext/{name}/{version}/*`.

**Install state machine (per extension, persisted in `plinth.packages.state`):**

```
  (admin uploads zip)
          │
          ▼
      UPLOADING         ── failure ──▶ rejected (404 / 400, nothing persisted)
          │
          ▼
      VALIDATING        ── failure ──▶ rejected (zip deleted, error to admin)
          │
          ▼
      MIGRATING         ── failure ──▶ INSTALL_FAILED (schema dropped, row flagged)
          │
          ▼
      REGISTERING       ── failure ──▶ INSTALL_FAILED (schema + migrations rolled back)
          │
          ▼
      EXTRACTING        ── failure ──▶ INSTALL_FAILED (partial asset dir removed)
          │
          ▼
      ACTIVATING        ── failure ──▶ INSTALL_FAILED (handlers unregistered)
          │
          ▼
      ACTIVE            ── Phase-B test failure ──▶ ACTIVE_FLAGGED
          │
     (disable/uninstall state transitions — see 0.4.5)
```

**Stages in detail:**

1. **UPLOADING.** Zip received via `POST /api/packages` (admin-authenticated, `packages.install` rule required). Saved to staging directory. Size and magic-number checks.

2. **VALIDATING.** Unzip to temp directory. Run 0.4.0 static validation and 0.4.2 cross-file validation (including runtime-state checks). Any error fails the install.

3. **MIGRATING.** Run 0.4.3. Extension schema is created if new; migrations run in order.

4. **REGISTERING.** Four parallel registrations, all within a single PG transaction:
   - Insert or update `plinth.packages` row (name, version, state=`REGISTERING`, provenance, manifest JSON, `frontend_mount`, `entry_point`, `installed_at`).
   - Register RBAC rules from `rbac.json` (rule validator — structural only) into `plinth.rbac_rules` (per ICD-0.1.4). This is 0.4.6's contract, orchestrated here.
   - Register capabilities from `capabilities.json` into `plinth.capabilities` (per DESIGN-capability-registry.md). This is the capability registry integration.
   - Register panels from `panels.json` into `plinth.panels` (new table — see §4).
   If any registration fails, the whole transaction rolls back.

5. **EXTRACTING.** Move validated files from temp to final location:
   - `{data_dir}/extensions/{name}/{version}/` — extension root
   - `{data_dir}/extensions/{name}/{version}/client/` — assets (served by the kernel at `/ext/{name}/{version}/*`)
   - `{data_dir}/extensions/{name}/{version}/server/` — backend code (loaded by QuickJS runtimes)
   - `{data_dir}/extensions/{name}/{version}/manifest.json` and friends (canonical copies)
   - Update `{data_dir}/extensions/{name}/active` symlink to point at `{version}/` (this is the atomic-swap point — see §8).

6. **ACTIVATING.** Register `/ext/{name}/{version}/*` static route handler with Drogon. If `frontend.mount` is set, register that mount. Capability handlers become callable (the QuickJS runtime pool picks up the extension's entry point on first call; see DESIGN-quickjs-bridge.md).

7. **ACTIVE.** Extension is operational. Phase-B RBAC integration tests run asynchronously (0.4.7). On failure, state moves to `ACTIVE_FLAGGED` — the extension keeps running but admin sees a warning.

**Asset-serving contract (from 0.4.4 onward):**

- Route: `GET /ext/{name}/{version}/{path:.*}`
- Resolution: `{data_dir}/extensions/{name}/{version}/client/{path}`
- Headers on success:
  - `Cache-Control: public, max-age=31536000, immutable`
  - `ETag: W/"{manifest-hash}"` (derived from manifest version + file mtime — immutable in practice, but ETag is standard)
  - `Content-Type:` from a static MIME table (`.js` → `application/javascript`, `.css` → `text/css`, etc.). Unknown extensions → `application/octet-stream`.
  - `Content-Security-Policy` inherited from the frontend's CSP (`architecture/06-frontend.md §3`).
- 404 on:
  - Unknown `{name}` or inactive `{version}` (old version after atomic swap drains)
  - Path attempting to escape via `..` (rejected before filesystem access)
  - Path outside `client/` (e.g., `/ext/notes/1.2.3/server/main.js` is 404, not a leak)

**Shell-specific first-boot install:**

The shell is bundled into the kernel binary as an embedded blob (CMake rule generates an object file from the shell source tree; linker embeds it). On kernel bootstrap, after schema creation and RBAC seeding:

```cpp
if (!is_frontend_installed()) {
    auto shell_blob = get_embedded_shell_package();
    install_package(shell_blob, /*provenance=*/"bundled");
}
```

`install_package()` runs the full state machine above. If the shell blob fails to install, bootstrap aborts with a clear error — an unbootable kernel is preferable to a half-bootstrapped one. Shell upgrades after first boot go through the normal admin-uploaded install path; the bundled blob is only consulted when no frontend is installed.

**Entry criteria:** 0.4.3 merged; capability registry (0.2.x) operational; QuickJS runtime pool (0.3.x) operational; ICD-0.1.5 RBAC enforcement available.
**Exit criteria:** A sample test extension installs through the full state machine; `/ext/{name}/{version}/main.js` is reachable with correct cache headers; the shell installs on first boot from the bundled blob; failure at every state correctly rolls back.

---

### 0.4.5 — Disable, Enable, Uninstall, Upgrade

**Scope:** All non-install lifecycle transitions. State machine continues from `ACTIVE`:

```
  ACTIVE ── admin disables ──▶ DISABLED ── admin re-enables ──▶ ACTIVE
  ACTIVE ── admin uninstalls ──▶ UNINSTALLING ──▶ (row deleted)
  ACTIVE ── admin uploads new version ──▶ (install lifecycle for new version, atomic swap on success)
  ACTIVE_FLAGGED ── any of the above (same transitions, flag cleared)
  INSTALL_FAILED ── admin removes ──▶ (row deleted, schema dropped)
```

**Disable:**

- Marks `plinth.packages.state = DISABLED`, sets `disabled_at`.
- Unregisters Drogon routes (assets no longer served, `frontend.mount` released if applicable).
- Unregisters capability handlers (calls to this extension's capabilities return `capability not found`).
- Marks RBAC rules **orphaned** per `architecture/01-identity.md §2` (`orphaned_at` set; rules remain assigned to groups but evaluate to deny).
- Does NOT drop the schema, does NOT remove files. Disable is reversible.

**Enable:**

- Precondition: state is `DISABLED`.
- Validates the package files still exist on disk and match the manifest checksum. If not, installation failed to clean up properly — error, admin resolves.
- Re-registers routes, handlers, panels. Clears `orphaned_at` on RBAC rules.
- Re-runs Phase-B tests (0.4.7). Failure moves state to `ACTIVE_FLAGGED`.

**Uninstall:**

- Precondition: any state except `UNINSTALLING`.
- `plinth.packages.state = UNINSTALLING`, marks `uninstalling_at`.
- **Removes RBAC rules entirely** (deletes from `plinth.rbac_rules`, removes all `plinth.group_rules` entries referencing them — per `architecture/01-identity.md §2` "Rule Lifecycle").
- Unregisters capabilities from the registry.
- Unregisters routes.
- **Drops the PG schema** (`DROP SCHEMA ext_{name} CASCADE`). This is irreversible; admin must confirm.
- Deletes `{data_dir}/extensions/{name}/` recursively.
- Deletes the `plinth.packages` row.

**Upgrade (install new version when a version is already active):**

- The new version goes through the full install lifecycle (0.4.4) from `UPLOADING` through `EXTRACTING`.
- Between `EXTRACTING` and `ACTIVATING`, the **atomic swap** happens:
  1. New version's assets are fully written to `{data_dir}/extensions/{name}/{new-version}/`.
  2. Capability handlers for the old version are drained (existing in-flight calls complete; no new calls accepted for ~100ms).
  3. `{data_dir}/extensions/{name}/active` symlink is atomically updated to point at `{new-version}/`.
  4. New version's handlers are registered; capability dispatch now routes to them.
  5. Old version's Drogon routes are unregistered; `/ext/{name}/{old-version}/*` starts returning 404 (intentional — forces browser cache invalidation).
  6. Old version's files remain on disk for a **retention window** (default: 24 hours) to catch stragglers; after the window, files are garbage-collected by a scheduled task (see 0.7.x scheduler).
- Migrations run before the swap. If a migration fails, the new version is never activated; the old version continues to run.
- RBAC rules are reconciled: new rules in the new version are registered; rules removed in the new version are marked orphaned (not deleted — admin must explicitly uninstall the old version's data).

**Concurrency:** All lifecycle transitions acquire a PG advisory lock keyed by package name. Simultaneous installs of the same package serialize; simultaneous operations on different packages proceed in parallel.

**Entry criteria:** 0.4.4 merged.
**Exit criteria:** All state transitions work correctly; upgrade preserves data (extension's PG tables survive a version bump if migrations don't destroy them); uninstall fully cleans up; concurrent operations on the same package serialize correctly.

---

### 0.4.6 — RBAC Rule Registration from `rbac.json` (rule validator)

**Scope:** The rule-registration step orchestrated by 0.4.4's REGISTERING stage. This sub-version is smaller than the others — it codifies the contract and adds the test-contract parser.

**`rbac.json` schema (permanent contract):**

```json
{
  "rules": [
    {
      "rule": "notes.edit",
      "namespace": "notes",
      "description": "Edit notes",
      "test": {
        "assert_deny": {
          "call": "notes:1:edit('hello')",
          "expect": "permission_denied"
        },
        "assert_allow": {
          "call": "notes:1:edit('hello')",
          "expect": "success"
        }
      }
    }
  ]
}
```

**Rule-validator checks (per `architecture/01-identity.md §2` "RBAC Test Validation — Two Phase"):**
- Every rule name follows convention: `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$`.
- Every rule's `namespace` matches either the package name or a reserved kernel namespace.
- Every test contract's `call` parses as a valid capability invocation string (uses the capability-string parser from 0.2.1).
- Every test contract's `call` references a capability that exists in the package's `capabilities.json` with matching version.
- No rule name collides with an already-registered rule from a different extension (except in the case of the same extension re-registering on upgrade).

**Storage:** Into `plinth.rbac_rules` per ICD-0.1.4. New column `test_contract JSONB` holds the full `test` object verbatim for use by 0.4.7.

**Entry criteria:** 0.4.5 merged; ICD-0.1.4 RBAC rule storage available; capability-string parser (0.2.1) available.
**Exit criteria:** Every rule-validator check has a test fixture; invalid fixtures produce structured errors; valid fixtures populate `plinth.rbac_rules` correctly.

---

### 0.4.7 — RBAC Integration Tests (post-install)

**Scope:** Run the actual `assert_deny` and `assert_allow` tests after an extension is active. This is the final piece of the 0.4 arc.

**Execution model:**

1. After an extension reaches `ACTIVE` state (from install or enable), a Phase-B test run is scheduled (immediate, asynchronous).
2. The kernel creates ephemeral test users scoped to this test run:
   - `__test_denied_{run_id}` — member of `everyone` only.
   - `__test_allowed_{run_id}` — member of `everyone` plus a synthetic group granted the rule under test.
   - Both users are created in `plinth.users` with a marker (`is_test_user = true`) and are excluded from all user-listing queries.
3. For each rule in the extension's `rbac.json`:
   - If `assert_deny` is declared: invoke the capability as `__test_denied_{run_id}`. Expect `permission_denied`. Any other result is a Phase-B failure.
   - If `assert_allow` is declared: invoke the capability as `__test_allowed_{run_id}`. Expect the call to complete without a permission error. (The capability may still fail for legitimate reasons — e.g., invalid arguments — but that's distinguishable from a permission failure.)
4. All test users are deleted after the run completes, success or failure.

**Result handling:**

- **All pass:** State stays `ACTIVE`. A `packages.rbac_test_passed` audit event is logged.
- **Any fail:** State moves to `ACTIVE_FLAGGED`. Admin is notified with the specific rules that failed and their expected vs. actual outcomes. The extension keeps running; the flag is a warning, not a disable.
- **Re-run on demand:** `plinth test rbac {extension}` or an admin-UI button re-runs the RBAC test. Success clears the flag.

**What the RBAC test does NOT do:**
- Does not invoke `assert_allow` with side effects — a capability that sends email or deletes files still does so during `assert_allow`. **Extensions are responsible for making their test-contract capabilities safe to run in test.** The extension guide documents this. Packages that fail to make their tests idempotent will discover this through audit logs and admin complaints.
- Does not guarantee the extension is correct — only that its declared rule contract is enforced at capability-dispatch time.

**Entry criteria:** 0.4.6 merged; ICD-0.1.5 RBAC enforcement path fully operational; capability dispatch (0.2.x) routes calls through the middleware.
**Exit criteria:** A test extension with a passing and a deliberately-broken rule installs; the RBAC test passes the good rule, flags the broken one; `plinth test rbac` re-runs and matches initial results; test users are cleaned up on both success and failure paths.

---

## 4. Data Model

### 4.1 New table: `plinth.packages`

Authoritative record of every installed or previously-installed package.

| Column | Type | Notes |
|--------|------|-------|
| `id` | UUID | PK, default `gen_random_uuid()` |
| `name` | TEXT | NOT NULL. Matches manifest `name`. Unique with `version` among non-uninstalled packages. |
| `version` | TEXT | NOT NULL. SemVer string. |
| `state` | TEXT | NOT NULL. Enum: `UPLOADING`, `VALIDATING`, `MIGRATING`, `REGISTERING`, `EXTRACTING`, `ACTIVATING`, `ACTIVE`, `ACTIVE_FLAGGED`, `DISABLED`, `INSTALL_FAILED`, `UNINSTALLING`. |
| `provenance` | TEXT | NOT NULL. Enum: `bundled` (shell on first boot), `user` (admin-uploaded). |
| `manifest_json` | JSONB | NOT NULL. Full manifest captured at install. |
| `frontend_mount` | TEXT | NULL unless `manifest.frontend.mount` is set. Unique (partial index WHERE NOT NULL AND state = 'ACTIVE'). |
| `entry_point` | TEXT | NOT NULL. From `manifest.entry_point`. |
| `manifest_checksum` | TEXT | NOT NULL. SHA-256 of the canonical manifest files, used for enable-time integrity check. |
| `installed_at` | TIMESTAMPTZ | NOT NULL, default `NOW()` |
| `disabled_at` | TIMESTAMPTZ | NULL unless state = `DISABLED` |
| `uninstalling_at` | TIMESTAMPTZ | NULL unless state = `UNINSTALLING` |
| `last_rbac_test_run_at` | TIMESTAMPTZ | NULL until first run |
| `last_rbac_test_result` | JSONB | NULL or `{passed: [...], failed: [...]}` |

**Uniqueness constraint:** `(name, version)` must be unique. Combined with the state machine, this means there can be at most one row per `(name, version)` pair. Upgrades delete the old-version row only after the retention window (§0.4.5 atomic swap).

**Partial index:** `UNIQUE (name) WHERE state IN ('ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED')` — at most one installed version of a package at a time.

**Partial index:** `UNIQUE (frontend_mount) WHERE frontend_mount IS NOT NULL AND state IN ('ACTIVE', 'ACTIVE_FLAGGED')` — at most one extension claims any given mount.

### 4.2 Existing table extended: `plinth.rbac_rules`

Add column `test_contract JSONB` (nullable). Populated by 0.4.6 from `rbac.json`'s `test` field. Used by 0.4.7 at runtime. No change to ICD-0.1.4's CRUD contract — this is an internal field, not exposed on the groups/rules API.

### 4.3 New table: `plinth.panels`

Registry of UI panels declared by extensions. Consumed by the shell to build the topbar and sub-tab structure (DESIGN-shell-v06x.md §3).

| Column | Type | Notes |
|--------|------|-------|
| `package_id` | UUID | FK → `plinth.packages.id`, ON DELETE CASCADE |
| `panel_id` | TEXT | NOT NULL. From `panels.json` entry `id` |
| `panel_type` | TEXT | NOT NULL. Enum: `primary`, `float`, `settings`, `tray` (per DESIGN-shell-v06x.md §3.1). New `tray` value lands with shell 0.6.6. |
| `slot_type` | TEXT | NULLABLE. For slot-filling panels (e.g. `home` launcher tile). Enum: `home` (per DESIGN-shell-v06x.md §2.4). |
| `declaration` | JSONB | NOT NULL. The full `panels.json` entry verbatim |
| `registered_at` | TIMESTAMPTZ | NOT NULL, default `NOW()` |

**Primary key:** `(package_id, panel_id)`.

The shell queries this table (via kernel API, not directly) to render the topbar tabs. Panels disappear from the shell UI when the package is disabled or uninstalled (CASCADE handles uninstall; disable is application-level).

### 4.4 Filesystem layout under `{data_dir}/extensions/`

```
{data_dir}/extensions/
  notes/
    1.2.3/
      manifest.json
      capabilities.json
      rbac.json
      panels.json
      server/
        main.js
        handlers/
          edit.js
          render.js
      client/
        main.js
        panels/
          editor.js
        css/
          styles.css
        assets/
          icon.svg
      migrations/
        001_create_notes.sql
    1.2.2/                    ← retained post-upgrade until retention window expires
      ...
    active -> 1.2.3           ← symlink, atomically updated
  shell/
    1.0.0/
      ...
    active -> 1.0.0
```

Filesystem is the source of truth for package *files*. `plinth.packages` is the source of truth for *state*. The two are reconciled by the enable-time checksum verification and by a scheduled consistency check (0.7.x).

---

## 5. Interface Contracts Between Versions

The 0.4.x arc produces the following contracts that later milestones depend on:

| Contract | Producer | Consumer | Stability |
|----------|----------|----------|-----------|
| `manifest.json` schema | 0.4.1 | Every extension, forever | Permanent |
| `frontend.mount` field | 0.4.1 | Shell (0.6), future BYO frontends | Permanent |
| `shareable[]` field (reserved) | 0.4.1 | 0.11 sharing arc | Permanent slot, semantics TBD |
| `capabilities.json` schema | 0.4.1 | Capability registry (0.2.x), QuickJS bridge (0.3.x) | Permanent |
| `rbac.json` schema | 0.4.6 | RBAC enforcement (ICD-0.1.5), Phase-B testing (0.4.7) | Permanent |
| `panels.json` schema | 0.4.4 | Shell (0.6.x) | Permanent — shell SDK lives on top of this |
| `plinth.packages` table shape | 0.4.4 | Admin UI (0.6.4), package management CLI, upgrade tooling | Permanent after 0.7 schema freeze |
| Install state machine | 0.4.4 / 0.4.5 | Any code that introspects package state (admin UI, metrics, audit) | Permanent |
| `/ext/{name}/{version}/*` URL shape | 0.4.4 | Every frontend, every extension client bundle | Permanent |
| Atomic-swap guarantee | 0.4.5 | Hot-reload (0.10.4), future blue-green upgrades | Permanent |

---

## 6. Dependencies Between Versions

```
0.4.0 ─┐
       ├─▶ 0.4.1 ─▶ 0.4.2 ─▶ 0.4.3 ─▶ 0.4.4 ─▶ 0.4.5
       │                                  │
       │                                  ├─▶ 0.4.6 ─▶ 0.4.7
       │                                  │
       └─ shared parser types ────────────┘
```

- 0.4.0 is a CLI tool; its only dependency on the broader arc is shared parser code with 0.4.1.
- 0.4.2 (cross-file validation) depends on both manifest parsers (0.4.1).
- 0.4.3 (schema/migrations) is independent of validation but orchestrated by 0.4.4.
- 0.4.4 is the orchestrator; it depends on everything before it and additionally on 0.2.x (capability registry) and 0.3.x (QuickJS bridge).
- 0.4.5 extends 0.4.4's state machine.
- 0.4.6 is the parser + storage piece of RBAC registration; logically subordinate to 0.4.4's REGISTERING stage, but split out for scope management.
- 0.4.7 depends on 0.4.6 (reads `test_contract`) and on a fully-live RBAC enforcement path.

External dependencies:
- 0.2.x must reach at least 0.2.2 (capability resolution) before 0.4.4 can activate extensions meaningfully.
- 0.3.x must reach at least 0.3.2 (kernel stdlib in QuickJS) before extension handlers can do anything useful.
- ICD-0.1.5 RBAC enforcement must be fully operational before 0.4.7 can run.

---

## 7. Out of Scope for 0.4

Per methodology Scale-2 design-doc requirements, these decisions are out of scope for the 0.4 arc. They fall into three groups, matching §2's committed/uncommitted breakdown with a third group for items already decided elsewhere. **The operational rule is the same for all three: do not make the decision in a 0.4 code session.** The reasoning differs, and the reasoning matters for how later arcs relate to 0.4.

### 7.1 Deferred but committed — 0.4 must not block

Features on the roadmap that 0.4 does not implement. 0.4's design must preserve the path.

- **Shell SDK versioning.** See `ARCHITECTURE.md §8`. Extensions don't declare "built against shell SDK 2.3" in 0.4. When this mechanism is added (probably 0.7+), it's an additive manifest field. 0.4's manifest parser must accept unknown top-level fields gracefully so additive additions don't require a parser rewrite.

- **Sidecar packaging (0.8.x).** Sidecars are on the roadmap. They may or may not share this manifest format — `DESIGN-sidecars-v08x.md` will either extend `manifest.json` or define a separate manifest file. 0.4 must not embed assumptions that would force one of those outcomes. Concretely: keep `server/` and `migrations/` handling in code paths that can be skipped cleanly for sidecar-style packages.

### 7.2 Deferred and uncommitted — 0.4 must not foreclose

Capabilities that might never ship. 0.4 reserves the slots they would need; if they never materialize, the slots sit empty forever and cost nothing.

- **Manifest schema migration mechanism.** The 0.4.1 schema is permanent, but when a future milestone needs to add *required* fields (not reserved slots), the mechanism for migrating old manifests is undecided. Do not add a `manifest_version` field in 0.4. If this is eventually needed, it will be introduced in a way that treats unversioned manifests as v1.

- **`shareable[]` semantics.** Reserved in 0.4.1 as an empty-only array. Its actual schema (handler paths, resource types, options) is defined in DESIGN-sharing-v011x.md if and when that arc is picked up — not a 0.4 decision.

- **Remote package registry.** Packages arrive as uploaded zips in 0.4 and remain so unless a future arc changes it. `ARCHITECTURE.md §8` open question. Do not build `POST /api/packages?from=https://...` or any network-fetch path in 0.4.

- **Package signing and provenance verification.** All packages are trusted-on-upload in 0.4. Signing, signature verification, trust policy — all deferred. The `provenance` field currently has two values (`bundled`, `user`); adding `signed-by-X` later is additive.

- **Sandboxed Phase-B execution.** Extensions are responsible for making `assert_allow` capabilities safe to execute. The kernel does not attempt to run them in a sandboxed or reversible mode in 0.4. Adding sandboxed Phase-B execution is a possible future optimization; it may never be needed.

### 7.3 Already decided elsewhere — 0.4 must not re-open

Questions that look like "0.4 decisions" but have already been answered. 0.4 implements the existing answer rather than re-litigating it.

- **Package-to-package dependencies.** Rejected in favor of capability requirements. Per DESIGN-capability-registry.md, packages declare what capabilities they provide and require; they do not declare which other packages they depend on. Do not add a `dependencies: ["other-package"]` field to `manifest.json`. Required capabilities go in `capabilities.json` under `requires`. This is the architecture replacing a legacy pattern (Armature's package deps) with a more composable one.

- **BYO frontend as a supported configuration.** Per `architecture/06-frontend.md §6`, the architecture permits an alternative frontend extension to claim `/`, but the project does not support, test, or document this configuration. 0.4 must implement `frontend.mount` correctly (the shell depends on it) but must not add admin-UI paths, compatibility testing, or documentation suggesting BYO is a target. If someone builds a replacement frontend, it works architecturally; keeping it working is their problem, not the project's.

---

## 8. Atomic Swap: Ordering Guarantee Detail

The upgrade swap in 0.4.5 has subtle ordering requirements that code sessions will get wrong without explicit guidance. Canonical sequence:

```
T0: New version written to {data_dir}/extensions/{name}/{new-version}/ (complete, fsynced)
T1: plinth.packages row for new version inserted, state = ACTIVATING
T2: Drain window starts:
      - New capability calls for this extension block for ≤100ms
      - In-flight calls continue against old handlers
T3: After drain, under a single PG transaction + filesystem rename:
      - active symlink repointed: {name}/active -> {new-version}
      - plinth.packages new-version state = ACTIVE
      - plinth.packages old-version state = SUPERSEDED (new state — add to the enum)
      - Drogon: old routes unregistered, new routes registered
      - Capability registry: old handlers unregistered, new registered
T4: Unblocked; new calls dispatch to new handlers
T5: Retention timer set for old version (24h default, then GC)
```

**Why this order:** Writing files before the DB row means a crash at T0.5 leaves orphan files (cleanup-able). Writing the DB row before the symlink swap means a crash at T1.5 leaves a row pointing at files that aren't yet active (detectable on restart, roll forward or clean up). Swapping the symlink before unregistering routes means a brief window where `/ext/{name}/{old}/main.js` still resolves — this is fine because the file still exists; it's the 404-after-swap that's the deliberate browser-cache-buster.

**Crash recovery:** On kernel start, any row in states `ACTIVATING`, `MIGRATING`, `REGISTERING`, `EXTRACTING`, or `UNINSTALLING` is reconciled:
- `ACTIVATING`/`REGISTERING`/`EXTRACTING`: if all files present and DB row consistent, advance to `ACTIVE`. Otherwise, mark `INSTALL_FAILED` and alert admin.
- `MIGRATING`: check `plinth.migrations` for this extension; if the latest migration has `applied_at IS NULL`, it failed mid-run — mark `INSTALL_FAILED`.
- `UNINSTALLING`: complete the uninstall (drop schema if not dropped, remove files, delete row).

This reconciliation runs synchronously at kernel bootstrap, before accepting any requests.

---

## 9. Milestone Criteria

**Arc entry criteria:**
- 0.1.x ICDs fully implemented (auth, groups, RBAC storage, RBAC enforcement, WebSocket, audit).
- 0.2.x capability registry operational through at least 0.2.2.
- 0.3.x QuickJS bridge operational through at least 0.3.2 (kernel stdlib).

**Arc exit criteria (end of 0.4.7):**
- All per-version exit criteria met.
- Sample test extension installs, runs a capability, is disabled, re-enabled, upgraded, and uninstalled without admin intervention beyond the commands.
- Shell extension installs on first boot from bundled blob and survives a restart.
- CI includes fixture extensions covering every manifest validation rule, every lifecycle state transition, and every Phase-B outcome (pass, fail, re-run).
- Human approval of implementation plan obtained for each 0.4.x task.
- No structural decisions made by code sessions; any ambiguity returns to an architecture session.
- All code session commits include tests; all lifecycle transitions have integration tests.

---

## 10. Open Questions (to be resolved during 0.4 implementation, not requiring a new architecture session)

1. **Exact retention window for old versions post-upgrade.** Default 24h proposed; could be configurable per-extension or per-deployment. Decide during 0.4.5 benchmarking.
2. **Admin UI uploader size limits vs. kernel limits.** `architecture/05-extensions.md §3` says 50MB default max package size. Admin-UI may need a lower default for browser upload UX. Not a contract decision.
3. **Test user naming collision.** `__test_denied_{run_id}` could theoretically collide with a real user named the same. Validate in 0.1.2 that `__test_` is a reserved user-name prefix, and reject registration of such names. File an amendment to ICD-0.1.2 if needed — small enough to handle without a new architecture session.
4. **MIME-type table for asset serving.** Static table in 0.4.4. Which types are included is a maintenance decision, not architectural. Start conservative (js, css, html, json, svg, png, jpg, jpeg, webp, woff2, gif); add on demand.
5. **GC cadence for old-version retention.** Either a 0.7.x scheduled task or a lazy-on-next-upgrade cleanup. Decide during 0.4.5.

---

## Appendix A: The `shareable[]` Reserved Slot

The 0.4.1 manifest parser accepts `shareable` as an optional field. It must be an array. In 0.4.x, it must be empty. Non-empty values produce a warning and are ignored.

This slot exists so that when DESIGN-sharing-v011x.md lands, the manifest schema doesn't need a breaking change. The field name, position, and "must be an array" constraint are locked in by this arc. Its contents are defined by the sharing design.

When 0.11 arrives, expected shape (from Architecture Appendix E.1):

```json
{
  "shareable": [
    {
      "resource_type": "note",
      "handler": "server/handlers/share_note.js"
    },
    {
      "resource_type": "file",
      "handler": "server/handlers/share_file.js"
    }
  ]
}
```

This is a 0.11 decision. 0.4 only reserves the slot.

---

## Appendix B: Example Package Lifecycle Timeline

Concrete walk-through for a hypothetical Notes extension, to anchor code sessions:

```
t=0s     Admin clicks "Install" in admin UI, selects notes-1.2.3.zip
t=0.1s   POST /api/packages lands; state=UPLOADING
t=0.2s   Zip extracted to /tmp/plinth-install-xxxx/
t=0.3s   0.4.0 static validation passes
         0.4.2 cross-file validation passes (no conflicts)
         state=VALIDATING → VALIDATING complete
t=0.4s   state=MIGRATING
         ext_notes schema created
         001_create_notes.sql runs (50ms)
         state=MIGRATING complete
t=0.5s   state=REGISTERING
         Single PG transaction:
           - plinth.packages row inserted (state=REGISTERING, manifest, etc.)
           - 3 RBAC rules registered in plinth.rbac_rules
           - 2 capabilities registered in plinth.capabilities
           - 1 panel registered in plinth.panels
         transaction commits
t=0.6s   state=EXTRACTING
         Files moved from /tmp to {data_dir}/extensions/notes/1.2.3/
         {data_dir}/extensions/notes/active created → 1.2.3
t=0.7s   state=ACTIVATING
         Drogon registers /ext/notes/1.2.3/* handler
         Capability registry marks notes:1:edit, notes:1:render as callable
         Panel registration visible to shell (shell's plinth.subscribe picks it up)
t=0.8s   state=ACTIVE
         Admin UI shows "notes 1.2.3 installed"
         Shell topbar gets a new tab (appearing live via realtime subscription)
t=0.9s   the RBAC test test run scheduled (async)
t=1.2s   the RBAC test: 3 rules × 2 assertions each = 6 test invocations
         All pass
         last_rbac_test_run_at set, last_rbac_test_result={passed: [...]}
         state remains ACTIVE
t=1.3s   Audit log: packages.installed{name=notes, version=1.2.3, admin=...}
         Audit log: packages.rbac_test_passed{name=notes, rule_count=3}
```

User opens the Notes tab in the shell. The shell loads `/ext/notes/1.2.3/main.js` (immutable cache, 200 from origin first time). Panel mounts. First capability call — `notes:1:edit` — flows through `RbacFilter` (denied until the user is in a group granted `notes.edit`). Admin grants `notes.edit` to the `authors` group. User is in `authors`. Next call succeeds.

Total time from click to first call: ~1 second plus network. Every step is observable in the audit log, persisted in `plinth.packages`, and rollback-able at every state.

---

**This document is the permanent authority for the 0.4.x arc.** Any code session working on 0.4.0 through 0.4.7 **must** read this document, `architecture/01-identity.md §2` (Groups and RBAC), `architecture/02-capabilities.md §1` (Capability Registry), `architecture/03-data.md §1` (Database), `architecture/05-extensions.md §1` (Package Structure), `architecture/05-extensions.md §2` (Reserved URL Prefixes), `architecture/05-extensions.md §3` (QuickJS Runtime), and `architecture/06-frontend.md` (Frontend Architecture) before beginning work. Structural decisions outside the scope defined here require a new architecture session and a revision of this document.