# ICD-0.4.5-package-lifecycle-transitions

**Traces to:** DESIGN-packages-v04x.md §0.4.5 (Disable / Enable / Uninstall / Upgrade — the authoritative contract for all non-install state transitions); DESIGN-packages-v04x.md §4.1 (`plinth.packages` columns — 0.4.5 writes `disabled_at` / `uninstalling_at`, adds `SUPERSEDED` state + `retired_at` + `supersedes_id`); DESIGN-packages-v04x.md §8 (Atomic Swap — the ordering guarantee for upgrade, normative for this milestone); architecture/01-identity.md §2.4 (Rule Lifecycle — normative for disable-orphans vs uninstall-deletes); architecture/05-extensions.md §2 (Reserved URL Prefixes — the `/api/packages/{id}` owner extends from 0.4.4 POST/GET into PATCH/DELETE here).
**Depends on:** ICD-0.4.4-package-install-lifecycle (the state machine being extended; `install_package` + `reconcile_in_flight_installs` + `asset_server::register_routes`/`unregister_routes` all reused); ICD-0.4.3-extension-schema-creation-and-migration (`drop_schema_and_migrations` called at uninstall); ICD-0.4.2-cross-file-manifest-validation (runtime-state RT1/RT2 checks against the in-process PG during upgrade validation; `ParsedPackage` consumed); ICD-0.4.1-manifest-parsing (full manifest parse during upgrade); ICD-0.4.0-package-structure-validation (structural validation during upgrade); ICD-0.2.0-capability-registry / ICD-0.2.4-capability-rbac (`unregister_capability` called at disable + uninstall; bulk `register_capability` re-run at upgrade REGISTERING); ICD-0.1.4-groups-rbac (`plinth.rbac_rules.orphaned_at` write at disable; DELETE at uninstall, with cascading `plinth.group_rules` cleanup); ICD-0.1.5-rbac-enforcement (orphaned rules must evaluate to deny — enforcement side already accounts for `orphaned_at`); ICD-0.1.7-audit (per-transition audit events).
**Milestone:** 0.4.5 — complete the package lifecycle beyond first install: disable, enable, uninstall, and upgrade (with atomic swap + 24h retention contract for a 0.7.x scheduler to consume). Does NOT implement rule validator RBAC validation extensions (0.4.6), RBAC test integration tests (0.4.7), the 0.7.x garbage-collection scheduler itself (ICD-0.4.5 specifies its contract, 0.7.x owns the cron + invocation), or admin-UI wiring (0.6a-A).
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** `migrations/schema.sql:123–128` (state CHECK — `SUPERSEDED` added in this milestone; `disabled_at` / `uninstalling_at` columns at lines 136–137 gain writers; `retired_at` + `supersedes_id` columns added); `src/kernel/packages/install_lifecycle.{hpp,cpp}` (extended with four new entry points + upgrade branch that reuses `install_package`'s existing path through EXTRACTING); `src/kernel/packages/asset_server.{hpp,cpp}` (route `register_routes` / `unregister_routes` reused; the map-guarded trampoline from 0.4.4 slice A permits per-version add/remove without Drogon route-table mutation); `src/kernel/packages/handlers.{hpp,cpp}` (gains `PATCH /api/packages/{id}` + `DELETE /api/packages/{id}` controllers); `src/kernel/packages/migrations.{hpp,cpp}` (0.4.3's `drop_schema_and_migrations` called at uninstall; `run_migrations` called pre-swap at upgrade); `src/kernel/rbac/rule_registrar.cpp` (new `delete_extension_rules` + `mark_extension_rules_orphaned` / `clear_extension_rules_orphaned` helpers; `upsert_extension_rule` already handles update-in-place via the `orphaned_at=NULL` clear per CHANGELOG 0.4.4 deviation note); `src/kernel/capabilities/registration.{hpp,cpp}` (new `unregister_capability(namespace, version, function, PGconn&)` surface — 0.4.4 only calls `register_capability`. **Implementation deviation (0.4.5 file placement):** the symbol shipped in `registration.{hpp,cpp}` rather than the ICD-named `resolution.{hpp,cpp}`, for symmetry with `register_capability_tx` already in `registration.cpp`. Public symbol name and signature unchanged. Documented in `RE-EVAL-0.4.x-arc-closeout.md §2.2`.); `DEFERRED.md` (I.18/I.19/I.20 HTTP-harness entry stays orthogonal — §2.1 of RE-EVAL-0.4.x-following-0.4.4; not absorbed here).

---

## Overview

0.4.4 shipped a first-install-only state machine: everything that enters `ACTIVE` stays there. 0.4.5 completes the machine. An admin can disable a package (reversibly parking it), re-enable it, uninstall it (fully cleaning up schema, files, rules), or upload a newer version of an already-installed package (upgrade with an atomic swap between the two). The last of these is the load-bearing primitive — `DESIGN-packages-v04x.md §8` specifies its ordering guarantee precisely, and this ICD turns that ordering into a concrete choreography that code sessions can implement.

This is the orchestrator's second half. 0.4.4 supplies `install_package` (UPLOADING → ACTIVE) and the reconciler. 0.4.5 adds four more entry points and one schema edit. It does not introduce any new validation rule, any new migration semantic, any new capability-registry shape, any new RBAC storage contract — all those surfaces are consumed, not extended.

**Scope:**

- **Schema edit.** `plinth.packages.state` CHECK gains one value: `SUPERSEDED` (old-version terminal after upgrade, holds while retention window counts down). Two new columns: `retired_at TIMESTAMPTZ` (set at the atomic-swap point; NULL except on SUPERSEDED rows) and `supersedes_id UUID REFERENCES plinth.packages(id)` (links a new-version row to the old-version row it replaced — used by the reconciler to detect mid-swap crashes). No partial-index change: `uniq_packages_name_active` already excludes `SUPERSEDED` by construction (the predicate lists `ACTIVE` / `ACTIVE_FLAGGED` / `DISABLED` only), so a SUPERSEDED row coexists with the new ACTIVE row for the same `name` without colliding.
- **Four new library entry points** in `src/kernel/packages/install_lifecycle.hpp`: `disable_package(id, ctx)`, `enable_package(id, ctx)`, `uninstall_package(id, ctx, confirm)`, and `upgrade_package(zip_blob, existing_id, ctx)`. All return `std::expected<PackageRecord, TransitionFailure>`.
- **One GC contract function (no scheduler)** in the same header: `garbage_collect_superseded_versions(retention_hours, ctx) -> std::expected<GcReport, GcFailure>`. Invocation is owned by a 0.7.x scheduler; 0.4.5 ships the function body so that 0.7.x lands as pure wiring.
- **Two new HTTP endpoints** in `handlers.{hpp,cpp}`: `PATCH /api/packages/{id}` (body `{"action": "disable" | "enable"}`, RBAC-gated on `packages.install`) and `DELETE /api/packages/{id}?confirm=true` (same gate; `?confirm=true` required to avoid accidental admin click-through).
- **Extended `POST /api/packages` semantics.** The existing 0.4.4 endpoint rejects a same-name upload with 409. In 0.4.5 the pre-INSERT collision check in UPLOADING is augmented: if a row exists with state `ACTIVE` or `ACTIVE_FLAGGED` and a strictly-earlier `version` per SemVer comparison, the upload enters the upgrade branch instead of rejecting. Same-version or older-version uploads still reject (409 `upgrade-version-not-newer`).
- **New atomic-swap choreography** — a named phase inside the upgrade path, between EXTRACTING and ACTIVATING. See §State Machine for the sequence. The swap covers: route unregister/register, capability handler unregister/register, RBAC rule reconciliation (new/missing/changed), active-symlink repoint, and row-state flip (old → SUPERSEDED with `retired_at`; new → ACTIVE).
- **RBAC reconciliation on upgrade.** Three cases per rule, comparing the v1 and v2 `rbac.json` by the namespaced `rule` string: new in v2 → INSERT; present in v1 but absent in v2 → `orphaned_at = NOW()` (preserve `plinth.group_rules` grants so a downgrade / re-enable restores them); present in both → UPDATE `description`, `test_contract`, `extension_name` in place; clear `orphaned_at` unconditionally for v2 rules (idempotent re-enable side-effect).
- **Crash recovery extensions.** `reconcile_in_flight_installs` from 0.4.4 handled the install-path states. 0.4.5 extends it to recognize: (a) a SUPERSEDED row with no matching ACTIVE row for the same name (swap crashed between old → SUPERSEDED and new → ACTIVE; replay the swap or back out); (b) an UNINSTALLING row (complete the uninstall or roll back to the pre-uninstall state); (c) an upgrade row at ACTIVATING with a non-NULL `supersedes_id` (mid-swap, replay from the last durable point).
- **PG advisory lock per package name** — the same `hashtextextended('plinth.packages.' || name, 0)` key used by 0.4.4 install. All four 0.4.5 transitions acquire the lock. Simultaneous disable + uninstall for the same name serialize; different-name transitions proceed in parallel. Upgrade's long lock-hold window (potentially seconds, not milliseconds) is explicitly documented as acceptable.
- **Per-transition audit events** — `packages.disabled` / `packages.enabled` / `packages.uninstall_confirmed` / `packages.uninstalled` / `packages.upgrade_started` / `packages.upgrade_swapped` / `packages.upgrade_completed`. Failure variants (`packages.disable_failed`, etc.) mirror the success set.
- **Test fixtures.** A new `tests/fixtures/lifecycle_transitions/` tree with 25 cases — see §Test Cases. `PLINTH_KERNEL_TESTS=ON` gated on the PG-backed cases; a unit-level file covers the GC contract and the RBAC reconciliation comparator without PG.

**Out of scope (deferred):**

- **rule validator RBAC validation extensions.** 0.4.6 scope. 0.4.5 still uses 0.4.4's minimal `upsert_extension_rule` shim for new-and-changed rules; 0.4.6 layers in test-contract parsing and structural checks.
- **RBAC test RBAC integration tests on enable or upgrade.** 0.4.7 scope. 0.4.5 leaves `plinth.packages.last_rbac_test_run_at` NULL on enable and on upgrade completion; 0.4.7 schedules the first RBAC test run. DESIGN §0.4.5's "Re-runs RBAC test tests (0.4.7)" bullet is a 0.4.7 obligation, not a 0.4.5 one.
- **The 0.7.x garbage-collection scheduler itself.** 0.4.5 ships `garbage_collect_superseded_versions` as a library entry with full semantics, and a unit-level test that directly invokes it. Calling it on a cadence, serving its output to an admin UI, and wiring its metrics to `plinth.metrics` are all 0.7.x concerns. 0.4.5 does NOT register a `drogon::HttpApp` timer, a CRON entry, or a `std::thread` for this.
- **Admin UI for disable/enable/uninstall/upgrade.** 0.6a-A ships the admin extension; it will POST/PATCH/DELETE against the endpoints 0.4.5 creates. 0.4.5 verifies via `curl`.
- **Deferred I.18 / I.19 / I.20 HTTP tests.** Orthogonal per RE-EVAL-0.4.x-following-0.4.4 §2.1 + DEFERRED.md §2026-04-20. These exercise 0.4.4's POST endpoint, not 0.4.5's PATCH/DELETE. 0.4.5's test matrix adds PATCH/DELETE coverage (D.* / U.* / X.* cases), which does share an HTTP fixture need; that fixture is built in 0.4.5 and the deferred I.18/I.19/I.20 migration onto it is a follow-up PR as originally scheduled.
- **Signed/remote/registry upgrades.** All deferred per DESIGN §7.2. Upgrade paths in 0.4.5 always originate from an admin multipart POST — same entry point as first install.
- **Hot-reload (0.10.4) developer UX.** DESIGN §2 flags hot-reload as a future feature built on top of 0.4.5's atomic-swap primitive. 0.4.5 must not introduce any design that breaks the hot-reload path; its `upgrade_package` entry is the exact primitive hot-reload will drive.
- **Downgrade / rollback.** Not supported. An admin who wants to revert uninstalls then reinstalls the older version — that works because uninstall is full-cleanup, and the older package's zip is independent. A fast-path "rollback the swap that just happened" is not in 0.4.5; the 24h retention window exists for GC, not for atomic rollback. If rollback becomes a goal it is a post-0.4.7 design session.
- **Per-extension runtime isolation changes.** 0.4.5 does not touch QuickJS pool lifecycles. Upgrade's handler-swap is at the capability registry layer (`register_capability` / `unregister_capability`), not at the runtime-pool layer. An in-flight JS call in the old handler runs to completion against the old entry-point file (still on disk during retention); the next capability dispatch resolves to the new handler because the registry has been updated. The pool itself is not drained or recycled.

---

## State Machine

0.4.4 ends at `ACTIVE`. 0.4.5 adds four outbound transitions from `ACTIVE`, two from `DISABLED`, crash-recovery-only transitions from `UNINSTALLING` and upgrade-ACTIVATING, and one new terminal state (`SUPERSEDED`).

```
                         (from 0.4.4: install lifecycle)
                                    │
                                    ▼
                              ┌──────────┐
                              │  ACTIVE  │◀──────────┐
                              └────┬─────┘           │
          ┌────────────────────────┼────────────────────────────┐
          │                        │                            │
          │ disable                │ admin uploads              │ admin uninstalls
          │ (PATCH action=disable) │ newer version              │ (DELETE ?confirm=true)
          ▼                        │ (POST /api/packages)       ▼
    ┌──────────┐                   │                       ┌──────────────┐
    │ DISABLED │                   │                       │ UNINSTALLING │
    └────┬─────┘                   │                       └───────┬──────┘
         │                         │                               │
         │ enable                  │ upgrade lifecycle             │ (row + files + schema deleted)
         │ (PATCH action=enable)   │ (install branch w/ swap)      │
         └──────┐                  │                               │
                │                  ▼                               ▼
                │           ┌─────────────┐                   (row gone)
                │           │ UPLOADING → │
                │           │ VALIDATING →│
                │           │ MIGRATING → │
                │           │ REGISTERING→│
                │           │ EXTRACTING  │
                │           └──────┬──────┘
                │                  │
                │                  │ atomic swap
                │                  ▼
                │           (old → SUPERSEDED, new → ACTIVATING)
                │                  │
                │                  ▼
                │           ┌────────────┐
                └──────────▶│   ACTIVE   │───── old version retires ────▶ ┌────────────┐
                            └────────────┘                                 │ SUPERSEDED │
                                                                           └──────┬─────┘
                                                                                  │
                                                                                  │ GC (0.7.x
                                                                                  │ scheduler invokes
                                                                                  │ garbage_collect_
                                                                                  │ superseded_versions)
                                                                                  ▼
                                                                             (row + files
                                                                              deleted after
                                                                              retention window)
```

**Every transition writes `plinth.packages.state`** before performing any side-effect that can't be rolled back from the write's absence. A crash at any point is recoverable from the row + on-disk state per §Crash Recovery.

### DISABLED (from ACTIVE / ACTIVE_FLAGGED)

Entry: `PATCH /api/packages/{id}` with `{"action": "disable"}`. RBAC `packages.install`. The admin context is captured for the audit event.

Stage body (single PG transaction except where noted):

1. SELECT the row `FOR UPDATE`. Reject if state ∉ `{ACTIVE, ACTIVE_FLAGGED}` — return `invalid-state-transition` (409). Acquire the name's advisory lock first; hold through step 7.
2. UPDATE `plinth.packages` row: `state = 'DISABLED'`, `disabled_at = NOW()`.
3. `plinth::rbac::mark_extension_rules_orphaned(name, conn)` — sets `orphaned_at = NOW()` on every `plinth.rbac_rules` row where `extension_name = name AND orphaned_at IS NULL`. `plinth.group_rules` rows are NOT touched (grants survive for re-enable).
4. `plinth::capabilities::unregister_capability` for each capability in the package's `capabilities.json` (`manifest_json` payload). Each unregister emits a `plinth.capabilities.unregistered` LISTEN/NOTIFY per ICD-0.2.3, invalidating caches on every node.
5. `asset_server::unregister_routes(name, version)` — removes the in-memory trampoline map entry. `/ext/{name}/{version}/*` begins returning 404.
6. If `manifest.frontend.mount` is set, unregister the SPA-fallback route under that mount; the `uniq_packages_mount_active` partial index predicate (which excludes DISABLED) means the mount is released for reuse.
7. COMMIT.
8. Emit audit event `packages.disabled{name, version, disabled_by_user_id}`. Release advisory lock.

On failure at any step: ROLLBACK, release lock, return structured error. State stays ACTIVE / ACTIVE_FLAGGED. No side effects outside the transaction — the route unregistrations in steps 5–6 are in-memory only and fully reversible within the same transaction window (the in-memory map is restored from the committed `plinth.packages` query on any re-registration attempt).

Disable does NOT drop the schema (`ext_{name}`), does NOT remove files (`{data_dir}/extensions/{name}/`). Both are preserved for enable.

### ACTIVE (from DISABLED — enable)

Entry: `PATCH /api/packages/{id}` with `{"action": "enable"}`. Same RBAC gate.

Stage body (single PG transaction except where noted):

1. SELECT the row `FOR UPDATE`. Reject if state ≠ `DISABLED` (`invalid-state-transition`, 409). Acquire advisory lock on name.
2. **Integrity check.** Recompute `manifest_checksum` over `{data_dir}/extensions/{name}/{version}/` canonical manifest file set (the same SHA-256 0.4.4's UPLOADING captured). Compare to `plinth.packages.manifest_checksum`. Mismatch → return `checksum-mismatch-on-enable` (422); do NOT auto-heal; the admin uninstalls and reinstalls from zip. Rationale: a checksum mismatch means someone edited the on-disk files out-of-band, and auto-repair would silently discard that edit.
3. `asset_server::register_routes(name, version, client_root)` — re-adds the in-memory trampoline map entry. `/ext/{name}/{version}/*` begins serving again.
4. If `manifest.frontend.mount` is set, re-register the SPA-fallback route.
5. `plinth::capabilities::register_capability` for each entry in `capabilities.json` (from `manifest_json`). Registry re-populates; LISTEN/NOTIFY propagates.
6. `plinth::rbac::clear_extension_rules_orphaned(name, conn)` — sets `orphaned_at = NULL` on every `plinth.rbac_rules` row where `extension_name = name`. Pre-existing `plinth.group_rules` grants become active again.
7. UPDATE `plinth.packages` row: `state = 'ACTIVE'`, `disabled_at = NULL`. (Not `ACTIVE_FLAGGED` — enable clears the flag; 0.4.7 RBAC test can re-flag on failure when it ships.)
8. COMMIT.
9. Emit audit event `packages.enabled{name, version, enabled_by_user_id}`. Release advisory lock.

0.4.7 will schedule RBAC test to run here. 0.4.5 leaves `last_rbac_test_run_at` NULL.

On failure: ROLLBACK (state stays DISABLED), structured error response, best-effort reversal of any already-registered route/handler (in-memory only; no persistent state written before COMMIT).

### UNINSTALLING (from ACTIVE / ACTIVE_FLAGGED / DISABLED / INSTALL_FAILED / SUPERSEDED)

Entry: `DELETE /api/packages/{id}?confirm=true`. RBAC `packages.install`. Missing `?confirm=true` → `confirmation-required` (409) — returned BEFORE any state change or audit event. The audit trail only records confirmed uninstalls.

Stage body (multi-transaction; each labeled):

1. **Pre-flight (no DB writes).** Reject if state = `UNINSTALLING` (409, `already-uninstalling`). Reject if `?confirm=true` absent (409, `confirmation-required`). SELECT the row, capture `name` and `version`. Acquire advisory lock on name.
2. **Transaction A — mark-uninstalling.** UPDATE `plinth.packages` row: `state = 'UNINSTALLING'`, `uninstalling_at = NOW()`. COMMIT.
3. Emit audit event `packages.uninstall_confirmed{name, version, uninstalled_by_user_id, prior_state}`. This is the destructive-action marker — from here on the uninstall will drive to completion or a recoverable mid-state.
4. **Route + capability unregister (in-memory, pre-transaction).** `asset_server::unregister_routes(name, version)`, unregister SPA mount if present, `plinth::capabilities::unregister_capability` bulk. These are reversible only via re-registration from DB state; after step 5 below they can no longer be reversed.
5. **Transaction B — rules + rows.** In a single PG transaction:
   a. `DELETE FROM plinth.group_rules WHERE rule_id IN (SELECT id FROM plinth.rbac_rules WHERE extension_name = ?)` — strips all group grants.
   b. `DELETE FROM plinth.rbac_rules WHERE extension_name = ?` — removes the rules entirely.
   c. `DELETE FROM plinth.panels WHERE package_id = ?` — explicit delete (CASCADE would also handle this via the FK, but explicit makes the audit-event enumeration clean).
   d. `DELETE FROM plinth.capabilities WHERE extension_name = ?` — the capability registry row-deletion (0.4.4's register_capability writer; unregister_capability from step 4 only drops the in-memory cache entry and emits NOTIFY — the actual row deletion lives here).
   COMMIT.
6. **Schema drop (separate transaction, outside the PG one above because `DROP SCHEMA ... CASCADE` can't be rolled back safely on a long-lived connection).** `plinth::packages::drop_schema_and_migrations(name, admin_conn)` — 0.4.3's library entry. Drops `ext_{name}` CASCADE, drops `ext_{name}_role` (with `DROP OWNED BY ... CASCADE` pre-step per CHANGELOG 0.4.3 deviation), deletes `plinth.migrations` rows for the extension. Irreversible.
7. **Filesystem cleanup.** `std::filesystem::remove_all({data_dir}/extensions/{name}/)`. Unlinks the entire tree including the `active` symlink and any SUPERSEDED version directories still in the retention window (by uninstalling the name, the admin implicitly uninstalls every version). Best-effort: a residual file (e.g. NFS-held open by a stale reader) is logged as a warning, not a failure — the `plinth.packages` row is already deleted in the next step, so re-install of the same name creates a clean tree.
8. **Transaction C — delete row.** `DELETE FROM plinth.packages WHERE id = ?`. The FK `plinth.panels.package_id` is already cleaned in step 5c (explicit), so CASCADE-delete is a no-op here.
9. Emit audit event `packages.uninstalled{name, version, prior_state}`. Release advisory lock.

**Crash recovery:** if the kernel crashes between any pair of steps, `reconcile_in_flight_installs` detects an UNINSTALLING row and completes the uninstall by re-running from the earliest uncompleted step:
- Rules still present (step 5 not COMMITted) → re-run steps 4–8.
- Schema still present (step 6 failed) → re-run steps 6–8.
- Files still present (step 7 failed) → re-run steps 7–8.
- Row still present (step 8 failed) → re-run step 8.
Each step is idempotent by construction: DELETE WHERE against a missing row is a no-op; DROP SCHEMA IF EXISTS equivalent is built into `drop_schema_and_migrations`; filesystem `remove_all` on a missing tree is a no-op. The reconciler makes no attempt to undo a partial uninstall.

### ACTIVE (upgrade — from ACTIVE / ACTIVE_FLAGGED, new version)

Entry: `POST /api/packages` with `multipart/form-data`, package zip for a name that already has an ACTIVE or ACTIVE_FLAGGED row whose `version` is strictly earlier per SemVer. Same auth + RBAC as 0.4.4's install.

The upgrade path reuses 0.4.4's install pipeline through EXTRACTING, then inserts an **atomic-swap** phase before ACTIVATING, then runs ACTIVATING for the new version and retires the old.

**UPLOADING (reused from 0.4.4, augmented).** Streaming, magic-number, path-traversal checks identical to 0.4.4. Minimal manifest parse to extract `name` / `version`. Acquire advisory lock on name. The augmented collision check:

```sql
SELECT id, version, state FROM plinth.packages
WHERE name = ?
  AND state IN ('ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED')
  AND uninstalling_at IS NULL
```

Cases:
- No row → first install (0.4.4 path).
- Row state = `DISABLED` → 409 `disabled-version-present` ("{name} is disabled at version {v}; re-enable or uninstall before upgrading"). Rationale: the semantics of "upgrade a disabled package" are ambiguous (does the new version inherit the disabled state?); punt to an explicit admin step.
- Row state ∈ `{ACTIVE, ACTIVE_FLAGGED}` and new version ≤ existing version (SemVer) → 409 `upgrade-version-not-newer`.
- Row state ∈ `{ACTIVE, ACTIVE_FLAGGED}` and new version > existing version → enter upgrade branch.

INSERT a new `plinth.packages` row with `state = 'UPLOADING'` and `supersedes_id = <existing_row_id>`. The existing row is UNCHANGED at this stage.

**VALIDATING (reused from 0.4.4).** Full 0.4.0 + 0.4.2 pass including runtime-state RT1/RT2/RT3. RT1 (name collision) does NOT fire because the existing row is the upgrade target, not a conflict — the validator consults `supersedes_id` to whitelist the predecessor row (NEW in 0.4.5; additive validator config field `ValidationConfig::upgrade_from_id`).

**MIGRATING (reused from 0.4.4, with upgrade semantic).** `run_migrations` from 0.4.3. New migration files run against the existing `ext_{name}` schema. The 0.4.3 library is monotonic (applied migrations don't re-apply; new migrations appended to `plinth.migrations`). Migration failure → upgrade aborts: the new-version row is marked `INSTALL_FAILED`, the existing row stays ACTIVE, `upgrade-migration-failed` returned; files on disk for the new version are cleaned up by the extraction-failure path that comes later (EXTRACTING never runs if MIGRATING fails). **Migration failure never rolls back successfully-applied migrations** — this is the "sticky migrations" rule from ICD-0.4.3; the admin resolves by either reverting the migration manually in PG or by shipping a new patch-version package that un-does the partial migration.

**REGISTERING (reused from 0.4.4, with RBAC reconciliation).** The new-version row's REGISTERING is identical to 0.4.4 for `plinth.capabilities` (register new; old still in registry until swap) and for `plinth.panels` (fresh rows linked to the new `package_id`; the old `package_id`'s rows will be deleted at the swap). For RBAC rules the behavior diverges from 0.4.4:

1. Load `plinth.rbac_rules` rows WHERE `extension_name = name`, build set V1_RULES keyed on `rule` string.
2. Load the new package's `rbac.json` rules, build set V2_RULES.
3. For each rule in V2_RULES:
   - If `rule` ∉ V1_RULES: INSERT new row. `orphaned_at = NULL`.
   - If `rule` ∈ V1_RULES: UPDATE the existing row in place (`description`, `test_contract`, `extension_name`); set `orphaned_at = NULL` (idempotent for already-cleared rows). Preserve the row's `id` so `plinth.group_rules` grants survive.
4. For each rule in V1_RULES \ V2_RULES: UPDATE SET `orphaned_at = NOW()`. `plinth.group_rules` grants are preserved; they evaluate to deny while orphaned (ICD-0.1.5's existing rule) and will re-activate if the rule reappears in a later version.

**EXTRACTING (reused from 0.4.4).** Files copied + fsynced to `{data_dir}/extensions/{name}/{new-version}/`. The `active` symlink is NOT yet touched — it still points at the old version. Staging unlinked.

**— Atomic Swap (new in 0.4.5) —**

At this point the new version's files are on disk and fsynced, the DB row is at state `EXTRACTING`, all REGISTERING-phase DB writes have committed, the capability registry has both old and new entries (dispatch still uses old handler via the `active` symlink and the map-guarded trampoline referencing the old path), and the existing `active` symlink still points at the old version. The swap choreography:

1. **T0 — Declare intent.** UPDATE new row: `state = 'ACTIVATING'`, `supersedes_id` already set from UPLOADING. At this point a crashing kernel sees two rows for the same name — one ACTIVE (old), one ACTIVATING (new) with `supersedes_id` pointing at the ACTIVE row. The reconciler uses this exact pair to detect mid-swap state and replay.
2. **T1 — Drain window.** Start a drain timer on the old version's capabilities. New capability calls to `{namespace}:{version}:{function}` for the old version's `{version}` block for up to `upgrade_drain_timeout_ms` (default 5000, configurable). In-flight calls complete against the old handler. The drain is implemented at the capability-dispatch layer (not at the route layer) because capability dispatch is the unit of "in-flight call" for upgrade purposes — asset-route requests are independent GETs and do not need draining.
3. **T2 — Drain resolution.**
   - All in-flight calls complete within the timeout → proceed to T3.
   - Timeout expires with in-flight calls outstanding → UPDATE new row `state = 'INSTALL_FAILED'`, `last_install_report` = `{kind: "upgrade-drain-timeout", outstanding: N}`. The new-version files and schema edits remain, but no swap occurs; the admin can retry the upgrade (a subsequent POST with the same zip will re-extract into the existing `{new-version}/` directory and try again — files are overwritten, idempotent). Drogon's graceful-shutdown equivalent is not involved; the drain is a capability-layer mechanism.
4. **T3 — Swap transaction (single PG + filesystem atomic rename).** Inside a single PG transaction:
   a. UPDATE old row: `state = 'SUPERSEDED'`, `retired_at = NOW()`.
   b. UPDATE new row: `state = 'ACTIVE'`.
   Outside the PG transaction but synchronized to it via advisory lock:
   c. Symlink flip: `symlink({new-version}, {staging}/active.tmp)` + `rename({staging}/active.tmp, {data_dir}/extensions/{name}/active)`. Atomic on POSIX filesystems when source and destination are on the same mountpoint; we require `{data_dir}` to be a single mountpoint (§Security Constraint 4).
   d. COMMIT the PG transaction immediately before the symlink rename's `rename()` syscall. The DB commit provides durability; the `rename()` provides atomicity for readers (asset-route handlers resolve the symlink on each request, so a reader either sees the old path or the new one, never a half-way state).
5. **T4 — Route + capability cutover.** `asset_server::unregister_routes(name, old_version)` — old `/ext/{name}/{old-version}/*` starts returning 404 (deliberate — forces browser cache invalidation). `asset_server::register_routes(name, new_version, new_client_root)` — new route live. If frontend mount differs between versions (rare but possible), unregister old mount and register new; if same, no change. `plinth::capabilities::unregister_capability` for each capability in the old-version manifest; the new-version capabilities are already in the registry from REGISTERING. Dispatch for subsequent calls immediately routes to new handlers.
6. **T5 — Retention timer.** `retired_at` on the old row is the logical retention-window start. 0.7.x's scheduler reads `retired_at` and applies the configured retention (default 24h). No timer is set in-process at 0.4.5 — see §Library Surface.
7. **ACTIVATING complete** — continue into the 0.4.4 ACTIVATING tail (audit `packages.installed`-variant event, now `packages.upgrade_completed`).

**On any swap-transaction failure** (T3 fails): PG transaction rolls back. If the rename already occurred but the commit fails, the symlink must be un-rolled by a compensating rename (`rename(previous_active_target, ...)`). The reconciler detects this via `(old row state, symlink target)` comparison at kernel start and corrects.

### SUPERSEDED (terminal until GC)

Terminal state for the old-version row after upgrade. Files and schema rows remain on disk / in the DB for the retention window. Capability handlers for this version are unregistered; asset routes at `/ext/{name}/{old-version}/*` return 404. A GC run (invoked by 0.7.x) deletes the `plinth.packages` row, unlinks the version subdirectory, and leaves `{data_dir}/extensions/{name}/active` untouched (still pointing at the current ACTIVE version).

**No outbound transitions from SUPERSEDED except via GC.** An admin cannot "restore" a superseded version — that is a reinstall scenario.

---

## HTTP Surface

```
POST   /api/packages                  — upload a zip (admin; from 0.4.4, upgrade semantics extended)
GET    /api/packages                  — list installed packages (from 0.4.4)
GET    /api/packages/{id}             — single package record (from 0.4.4)
PATCH  /api/packages/{id}             — disable or enable (NEW in 0.4.5)
DELETE /api/packages/{id}?confirm=...  — uninstall (NEW in 0.4.5)
GET    /ext/{name}/{version}/*        — static asset (from 0.4.4)
```

### POST /api/packages (extended for upgrade)

Auth / RBAC / request shape: unchanged from 0.4.4. The response shape is unchanged on first install. On upgrade the response body adds a `supersedes` block:

```json
{
  "id": "7fe1...-new",
  "name": "notes",
  "version": "1.3.0",
  "state": "ACTIVE",
  "provenance": "user",
  "installed_at": "2026-05-01T12:00:00Z",
  "frontend_mount": null,
  "supersedes": {
    "id": "3c7e8b1e-...-old",
    "version": "1.2.3",
    "retired_at": "2026-05-01T12:00:03Z"
  },
  "rbac_reconciliation": {
    "added": ["notes.comment"],
    "updated": ["notes.edit"],
    "orphaned": ["notes.share"]
  },
  "warnings": []
}
```

HTTP status codes extended beyond 0.4.4's table:

| Failure condition | Status |
|---|---|
| UPLOADING — upgrade with same or older version | 409 `upgrade-version-not-newer` |
| UPLOADING — target is DISABLED | 409 `disabled-version-present` |
| MIGRATING — migration failed during upgrade | 422 `upgrade-migration-failed` |
| Atomic swap — drain timeout | 504 `upgrade-drain-timeout` |
| Atomic swap — unexpected post-EXTRACTING failure | 500 `upgrade-swap-failed` |

### PATCH /api/packages/{id}

Auth: session or PAT. RBAC `packages.install` (same rule as install; a principal that can install can also disable and enable — 0.4.5 does NOT split these into separate rules because the semantics are the same "mutate the package state" capability; a future milestone may introduce `packages.disable` if needed).

Request body:
```json
{ "action": "disable" | "enable" }
```

Response on success (200):
```json
{
  "id": "...",
  "name": "notes",
  "version": "1.2.3",
  "state": "DISABLED",
  "disabled_at": "2026-05-02T09:15:00Z"
}
```

Status codes:
| Condition | Status |
|---|---|
| Success — disable or enable | 200 |
| Unknown id | 404 |
| Body not valid JSON or action ∉ {disable,enable} | 400 `invalid-action` |
| Current state incompatible (e.g. disable on DISABLED) | 409 `already-disabled` / `already-enabled` / `invalid-state-transition` |
| Enable with checksum mismatch | 422 `checksum-mismatch-on-enable` |
| Missing RBAC rule | 403 |
| Advisory lock held (another transition in flight for same name) | 409 `in-flight-operation` |

### DELETE /api/packages/{id}

Auth / RBAC identical to PATCH. Query parameter `confirm=true` required.

Status codes:
| Condition | Status |
|---|---|
| Success — uninstall completed | 204 |
| Unknown id | 404 |
| `?confirm=true` absent | 409 `confirmation-required` |
| Current state = UNINSTALLING | 409 `already-uninstalling` |
| Missing RBAC rule | 403 |
| Advisory lock held | 409 `in-flight-operation` |
| Mid-uninstall filesystem residual (warning only) | 204 with `{warnings: [...]}` body |

Response on success (204 No Content) with no body is the default. If a warning arose during cleanup (step 7 residual), the status is 204 and the response body is `{"warnings": ["..."]}`.

### GET /ext/{name}/{version}/* (behavior extended)

Unchanged request shape. Two behavior extensions:

1. **DISABLED packages 404.** If the route's `(name, version)` pair is in the asset_server map, the request serves; if the package has been disabled (map entry removed), the request 404s. Browsers with cached responses honor `Cache-Control: immutable`, so a disable does not invalidate already-fetched JS in a live session — that is acceptable for a reversible operation. An uninstall does invalidate cached responses because the JS reappearing would be a security surprise; but since the URL's `{version}` is version-scoped and uninstall deletes the files, the 404 is cache-busting-sufficient.
2. **SUPERSEDED packages 404 at the old version's URL.** Post-upgrade, `/ext/notes/1.2.3/main.js` returns 404; `/ext/notes/1.3.0/main.js` serves. This is the deliberate browser-cache-buster noted in DESIGN §8. In-flight fetches against the old URL that landed after the swap will 404 — front-end code must retry at the new URL (the shell does this via the realtime panels subscription).

---

## Data Model

### `plinth.packages` — schema edits in this milestone

Three changes to the 0.4.4 `plinth.packages` definition:

```sql
-- 1. Extend the state CHECK with SUPERSEDED (at most one new value, no migration
--    of existing rows required — 0.4.4 never wrote any row into SUPERSEDED).
ALTER TABLE plinth.packages
    DROP CONSTRAINT packages_state_check,
    ADD CONSTRAINT packages_state_check CHECK (state IN (
        'UPLOADING', 'VALIDATING', 'MIGRATING',
        'REGISTERING', 'EXTRACTING', 'ACTIVATING',
        'ACTIVE', 'ACTIVE_FLAGGED',
        'DISABLED', 'INSTALL_FAILED', 'UNINSTALLING',
        'SUPERSEDED'
    ));

-- 2. Add retired_at (set at atomic-swap T3; NULL except on SUPERSEDED rows).
ALTER TABLE plinth.packages
    ADD COLUMN retired_at TIMESTAMPTZ;

-- 3. Add supersedes_id (set at UPLOADING in the upgrade branch; NULL on first-install rows).
ALTER TABLE plinth.packages
    ADD COLUMN supersedes_id UUID REFERENCES plinth.packages(id) ON DELETE SET NULL;

CREATE INDEX idx_packages_supersedes ON plinth.packages(supersedes_id)
    WHERE supersedes_id IS NOT NULL;
```

Since 0.4 schema is still fluid (per ROADMAP preamble: "0.1–0.6 schema is fluid; edit schema.sql directly"), these land as direct edits to `migrations/schema.sql` rather than a numbered migration. The `ALTER` syntax above is presentational — the shipped edit rewrites lines 123–128 of `schema.sql` to include the extra enum value, and appends the two `ADD COLUMN` statements to the table definition.

Column notes:
- `retired_at` is NULL on every state except SUPERSEDED. The contract is one-way: once a row enters SUPERSEDED, `retired_at` is set and never cleared. GC reads this column to determine eligibility.
- `supersedes_id` points at the row the new version replaced. The FK ON DELETE SET NULL means a GC run that deletes the old SUPERSEDED row doesn't need to touch `supersedes_id` on the new ACTIVE row — PG nulls it automatically. This also means the reconciler must tolerate `supersedes_id IS NULL` on a SUPERSEDED row (happens if the ACTIVE child was deleted first, e.g. uninstall-during-retention-window).
- The existing `uniq_packages_name_active` partial index already excludes SUPERSEDED (the predicate is `state IN ('ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED')`), so a SUPERSEDED row coexists with a new ACTIVE row for the same `name` without constraint violation. **No index predicate change is needed.** Same for `uniq_packages_mount_active`: SUPERSEDED old versions relinquish their mount claim on the atomic swap, and the new ACTIVE version claims it (the PATCH from ACTIVATING → ACTIVE does this transparently).

### `plinth.rbac_rules` — writers added

0.4.4 added the column `orphaned_at`. 0.4.5 is the first writer:
- Disable path: `UPDATE plinth.rbac_rules SET orphaned_at = NOW() WHERE extension_name = ? AND orphaned_at IS NULL`.
- Enable path: `UPDATE plinth.rbac_rules SET orphaned_at = NULL WHERE extension_name = ?`.
- Upgrade path: orphan-if-missing on a per-rule basis (UPDATE SET `orphaned_at = NOW()` WHERE `rule` not in v2 set); clear-to-null for every rule in v2 set (idempotent).
- Uninstall path: DELETE entirely.

No schema edit to `plinth.rbac_rules` in this milestone.

### `plinth.group_rules` — writer behavior

0.4.5 uninstall explicitly deletes `plinth.group_rules` rows referencing the uninstalled extension's rules (step 5a of the UNINSTALLING stage body). Disable does NOT touch `plinth.group_rules` — grants survive re-enable. Upgrade does NOT touch `plinth.group_rules` — grants survive upgrade even when rules transition to orphaned.

### `plinth.panels` — writer behavior

0.4.5 uninstall explicitly deletes `plinth.panels` rows (step 5c). CASCADE would also work but explicit is clearer for the per-step audit enumeration. Upgrade re-creates panel rows in the new package_id's REGISTERING (0.4.4 behavior); the old package_id's rows are deleted at swap T4 by the `DELETE FROM plinth.panels WHERE package_id = <old_id>` issued inline with the unregister steps. Disable does NOT touch `plinth.panels` — panel rows persist (the shell queries them via a capability that will filter on package state).

### `plinth.capabilities` — writer behavior

0.4.5 uninstall deletes capability rows. Upgrade replaces them (unregister old; register new — both at REGISTERING for new and swap T4 for old). Disable unregisters (deletes the cache entry + row) because DISABLED packages must not be dispatchable. Enable re-registers.

### `plinth.migrations` — writer behavior

0.4.5 uninstall deletes migration rows for the extension (via `drop_schema_and_migrations`). Upgrade appends new migrations to the existing `plinth.migrations` rows — sticky per ICD-0.4.3. Disable / enable do not touch `plinth.migrations`.

---

## Library Surface

```cpp
// src/kernel/packages/install_lifecycle.hpp — EXTENDED from 0.4.4
// (new entries added below the 0.4.4 set; 0.4.4 surface is unchanged.)

namespace plinth::packages {

enum class TransitionKind {
    DISABLE, ENABLE, UNINSTALL, UPGRADE,
};

struct TransitionFailure {
    TransitionKind kind;
    std::string package_id;           // UUID of the plinth.packages row (may be empty on UPGRADE if UPLOADING failed pre-INSERT)
    std::string message;
    nlohmann::json report;            // structured error detail (taxonomy below)
};

struct RbacReconciliation {
    std::vector<std::string> added;    // rule names new in v2
    std::vector<std::string> updated;  // present in both — fields updated in place
    std::vector<std::string> orphaned; // in v1, absent in v2 — orphaned_at set
};

struct UpgradeReport {
    PackageRecord new_record;
    std::string superseded_id;         // UUID of the old row, now SUPERSEDED
    std::chrono::system_clock::time_point retired_at;
    RbacReconciliation rbac;
    std::vector<std::string> warnings;
    std::chrono::milliseconds drain_waited;
};

// --- New entry points ---

auto disable_package(std::string_view package_id, InstallerContext& ctx)
    -> std::expected<PackageRecord, TransitionFailure>;

auto enable_package(std::string_view package_id, InstallerContext& ctx)
    -> std::expected<PackageRecord, TransitionFailure>;

auto uninstall_package(std::string_view package_id,
                       bool confirmed,
                       InstallerContext& ctx)
    -> std::expected<void, TransitionFailure>;

auto upgrade_package(std::span<const std::byte> zip_blob,
                     std::string_view existing_package_id,
                     InstallerContext& ctx)
    -> std::expected<UpgradeReport, TransitionFailure>;

// --- GC contract (body shipped in 0.4.5; invocation owned by 0.7.x scheduler) ---

struct GcReport {
    std::vector<std::string> collected_ids;  // UUIDs of removed SUPERSEDED rows
    std::vector<std::string> skipped_ids;    // rows eligible by time but locked/in-flight
    std::size_t bytes_freed;
    std::vector<std::string> warnings;       // filesystem residuals etc.
};

struct GcFailure {
    std::string message;
    std::vector<std::string> partial_collected_ids;
};

auto garbage_collect_superseded_versions(std::chrono::hours retention,
                                         InstallerContext& ctx)
    -> std::expected<GcReport, GcFailure>;

// --- Crash-recovery extension ---

// reconcile_in_flight_installs (0.4.4) gains three new cases:
//   (a) UNINSTALLING — continue the uninstall from the earliest incomplete step
//   (b) upgrade ACTIVATING with supersedes_id set — replay swap from last durable point
//   (c) SUPERSEDED orphan (supersedes_id points at a deleted child) — mark for GC
// Function signature unchanged; only the body grows.

}  // namespace plinth::packages
```

### Upgrade drain configuration

Added to `InstallerContext` (or a companion config struct):
```cpp
struct InstallerContext {
    // ... 0.4.4 fields
    std::chrono::milliseconds upgrade_drain_timeout_ms { 5000 };
};
```
Configurable via the `packages.upgrade_drain_timeout_ms` config key. Default 5000ms is a round figure; §Open Questions #1 leaves the tuning-to-telemetry question for implementation time.

### RBAC helpers

New functions in `src/kernel/rbac/rule_registrar.hpp`:

```cpp
namespace plinth::rbac {

auto mark_extension_rules_orphaned(std::string_view extension_name, PGconn& conn)
    -> std::expected<std::size_t, std::string>;   // returns count of rows touched

auto clear_extension_rules_orphaned(std::string_view extension_name, PGconn& conn)
    -> std::expected<std::size_t, std::string>;

auto delete_extension_rules(std::string_view extension_name, PGconn& conn)
    -> std::expected<std::size_t, std::string>;   // returns count of rows deleted

}  // namespace plinth::rbac
```

The existing `upsert_extension_rule` from 0.4.4 handles the "update-in-place, clear orphaned_at" semantic already; no signature change.

### Capability unregister

New function in `src/kernel/capabilities/resolution.hpp`:

```cpp
namespace plinth::capabilities {

auto unregister_capability(std::string_view ns,
                           int version,
                           std::string_view function,
                           PGconn& conn)
    -> std::expected<void, std::string>;

}  // namespace plinth::capabilities
```

Deletes the `plinth.capabilities` row and fires LISTEN/NOTIFY for cache invalidation. The registry-cache side (ICD-0.2.3) already subscribes to delete events; no cache-code changes required.

---

## Error Taxonomy

Surfaced in `TransitionFailure.report` as structured JSON. Top-level `kind`:

| `kind` | Transition | Condition |
|---|---|---|
| `invalid-state-transition` | disable / enable / uninstall | Current state not in the transition's allowed-set. |
| `already-disabled` | disable | State is already DISABLED. |
| `already-enabled` | enable | State is already ACTIVE or ACTIVE_FLAGGED. |
| `already-uninstalling` | uninstall | State is already UNINSTALLING. |
| `confirmation-required` | uninstall | `?confirm=true` not in request query. |
| `checksum-mismatch-on-enable` | enable | Re-computed checksum doesn't match `plinth.packages.manifest_checksum`. |
| `in-flight-operation` | any | Advisory lock held by another transition for the same name. |
| `upgrade-version-not-newer` | upgrade | SemVer compare: incoming version ≤ current version. |
| `disabled-version-present` | upgrade | Upgrade attempted when current row is DISABLED (not ACTIVE). |
| `upgrade-migration-failed` | upgrade | Migration failure during upgrade's MIGRATING stage. |
| `upgrade-drain-timeout` | upgrade | Drain window expired with in-flight capability calls outstanding. |
| `upgrade-swap-failed` | upgrade | Unexpected post-EXTRACTING failure during the atomic-swap transaction. |
| `schema-drop-failed` | uninstall | `DROP SCHEMA ext_{name} CASCADE` failed (unusual — PG permissions issue). |
| `filesystem-residual` | uninstall | Best-effort `remove_all` left residual files (reported as warning, not hard failure). |
| `reconcile-mid-swap` | crash recovery (informational) | Reconciler detected a mid-swap state and replayed or backed out — not a failure per se, just a log-surfaced event. |

All failure events log to `plinth.audit_log` as `packages.{transition}_failed{name, version, kind}`.

---

## Security Constraints

1. **Uninstall is destructive and requires explicit confirmation.** The `?confirm=true` query param is checked before any state mutation or audit event. A request without `confirm` is a pure read — no lock acquired, no row touched. This prevents CSRF or accidental-click attacks from uninstalling packages.

2. **Upgrade cannot downgrade.** SemVer comparison is strict-greater-than: same version is rejected (`upgrade-version-not-newer`). This prevents an attacker with install credentials from quietly replacing a newer signed package with an older vulnerable one. (Full provenance / signature verification is deferred per DESIGN §7.2; the version-monotone rule is the 0.4.5-era defense.)

3. **DROP SCHEMA ... CASCADE is the only path to schema removal.** Disable does NOT drop. Uninstall does, and only after confirmation. `drop_schema_and_migrations` is the single library entry — callers cannot bypass it by issuing their own `DROP SCHEMA`. The PG role that uninstall runs under (admin) has the privilege; the extension's own PG role (`ext_{name}_role`) does not, so a compromised extension cannot self-uninstall its own schema.

4. **Atomic-swap symlink rename requires same-filesystem `{data_dir}`.** POSIX `rename()` is atomic only within a single mountpoint. If `{data_dir}` straddles mountpoints (e.g. `{data_dir}/extensions/notes/` is on a different FS than `{data_dir}/extensions/notes/staging/`), the rename falls back to copy + unlink, which is NOT atomic and leaves windows where readers see the wrong target. Operational constraint: `{data_dir}` must be a single mountpoint. Kernel bootstrap checks this at startup (new in 0.4.5, added to the existing bootstrap self-checks) and refuses to start if `{data_dir}/extensions/` is on a different filesystem from `{data_dir}/extensions/staging/` — or in a simpler form, `{data_dir}` root is on one mountpoint.

5. **Advisory-lock hold window on upgrade is bounded.** The lock is held from UPLOADING through swap T4. Worst case is `upload_time + validate_time + migrate_time + upgrade_drain_timeout_ms + swap_time`, with drain dominating. Default 5s drain caps the pathological case at ~10s; if an operator configures a large drain timeout, they accept the serialization cost. Different-name upgrades proceed in parallel — the advisory lock is name-keyed.

6. **Route deregistration is in-memory-only.** 0.4.4's map-guarded trampoline approach (slice A deviation #1 per CHANGELOG v0.4.4) means `asset_server::unregister_routes` is a map delete, not a Drogon API call. This has two consequences: (a) a kernel restart re-registers all ACTIVE packages' routes from `plinth.packages` + filesystem evidence via `restore_routes`, and (b) there's no Drogon-internal state to leak across disable/enable cycles. The 0.4.4 `cancel_all_registrations` function (called at shutdown) remains the only place that clears the whole map.

7. **Orphaned RBAC rules evaluate to deny.** ICD-0.1.5's `RbacFilter` checks `orphaned_at IS NOT NULL` on a rule and denies if set (this is ICD-0.1.5's contract, already implemented for other orphan-source scenarios). A disabled package's rules therefore produce the same effect as an uninstalled one at the capability-call layer, even though the rules physically exist. Re-enable clears `orphaned_at` atomically with the rest of the re-enable transaction.

8. **Uninstall removes group grants explicitly before deleting rules.** The `DELETE FROM plinth.group_rules` step precedes `DELETE FROM plinth.rbac_rules`. Order is enforced by the transaction sequencing, not by FK CASCADE. The reason is audit-granularity: each `group_rules` row revocation is intended to be visible in the audit log as a distinct event (this is a per-row audit emission — not implemented at 0.4.5 but the schema supports it; 0.1.7's audit logger already handles per-rule emission for admin-initiated revokes and will apply transparently here). CASCADE would delete the rows but not emit distinct audit events.

9. **Retention window does NOT imply rollback capability.** SUPERSEDED files remain on disk for 24h as a safety net against stragglers (browsers with cached URLs, in-flight reloads), not for admin rollback. There is no admin-facing "restore superseded version" operation. If an operator wants rollback behavior they uninstall the new version and reinstall the old from its zip.

10. **GC cannot collect rows whose advisory lock is held.** `garbage_collect_superseded_versions` tries each candidate row's advisory lock non-blockingly; if held (some other transition is mid-flight on the same name), it skips and records the id in `skipped_ids`. Next run picks it up. A lock-holder will clear the lock eventually; GC is eventually-consistent by design.

---

## Test Cases

Fixtures under `tests/fixtures/lifecycle_transitions/` — separate tree from 0.4.4's `install_lifecycle/` because 0.4.5 transitions start from valid ACTIVE packages (the fixtures pre-install via 0.4.4's `install_package` during test setup; they are not "zips to validate" like 0.4.4's fixture set). Target count: **~32 cases** across four prefixes (D.* + U.* + X.* + G.*).

All tests gated `PLINTH_KERNEL_TESTS=ON` unless marked otherwise.

### Disable / Enable (D.*)

| # | Case | Expected |
|---|---|---|
| D.01 | Disable ACTIVE package | State → DISABLED; `disabled_at` set; asset route gone (404); capability unregistered; RBAC rules `orphaned_at` set; schema intact; files intact |
| D.02 | GET /ext/{name}/{version}/* on DISABLED package | 404 |
| D.03 | Capability call into DISABLED package | `capability-not-found` from registry |
| D.04 | Enable DISABLED package | State → ACTIVE; `disabled_at` cleared; routes restored; capability re-registered; RBAC rules `orphaned_at` cleared |
| D.05 | Enable after out-of-band file tamper (checksum mismatch) | State stays DISABLED; 422 `checksum-mismatch-on-enable`; no partial re-register |
| D.06 | Disable on ACTIVE_FLAGGED | Same as D.01 with input state = ACTIVE_FLAGGED; output state = DISABLED (flag not preserved through disable-enable cycle by design — 0.4.7 will re-run RBAC test on enable) |
| D.07 | Disable DISABLED → 409 `already-disabled` | No state change |
| D.08 | Enable ACTIVE → 409 `already-enabled` | No state change |
| D.09 | RBAC enforcement: user with notes.edit grant, package DISABLED | Capability call denied; `orphaned_at` check path verified |

### Uninstall (U.*)

| # | Case | Expected |
|---|---|---|
| U.01 | Uninstall ACTIVE without `?confirm=true` | 409 `confirmation-required`; no state change; no audit event |
| U.02 | Uninstall ACTIVE with `?confirm=true` | State → UNINSTALLING → row deleted; schema dropped; files removed; RBAC rules + group grants removed; capabilities unregistered; panels removed; audit `packages.uninstall_confirmed` + `packages.uninstalled` events |
| U.03 | Uninstall DISABLED with confirm | Same destructive path as U.02; prior_state recorded as DISABLED in audit |
| U.04 | Uninstall INSTALL_FAILED with confirm | Row + files + any orphan schema cleaned; `drop_schema_and_migrations` no-ops on missing schema gracefully |
| U.05 | Uninstall SUPERSEDED with confirm (within retention window) | Row deleted; `{data_dir}/extensions/{name}/{old-version}/` removed; the current ACTIVE row for same name is UNTOUCHED; `supersedes_id` nulled on ACTIVE via FK ON DELETE SET NULL |
| U.06 | Crash mid-UNINSTALLING (after Tx A, before Tx B) | Reconciler completes uninstall on kernel restart; final state matches U.02 end-state |
| U.07 | Crash mid-UNINSTALLING (after schema drop, before filesystem cleanup) | Reconciler re-runs filesystem cleanup + row delete; idempotent |

### Upgrade / Atomic Swap (X.*)

| # | Case | Expected |
|---|---|---|
| X.01 | Upload v1.3.0 when v1.2.3 is ACTIVE | Enters upgrade branch; new row inserted with `supersedes_id` pointing at old row; flows through UPLOADING → ACTIVATING → swap → ACTIVE |
| X.02 | Upload same version | 409 `upgrade-version-not-newer`; no new row |
| X.03 | Upload older version | 409 `upgrade-version-not-newer` |
| X.04 | Upload against DISABLED package | 409 `disabled-version-present`; no new row |
| X.05 | Upgrade with new migrations | New migrations appended to `plinth.migrations`; existing `ext_{name}` schema unchanged except for migration additions; applied_at set |
| X.06 | Upgrade migration failure | New row → INSTALL_FAILED; old row → ACTIVE (untouched); 422 `upgrade-migration-failed`; old schema intact with any successfully-applied pre-failure migrations (sticky per ICD-0.4.3) |
| X.07 | Upgrade RBAC reconciliation — new rule + missing rule + changed rule | New: INSERT with orphaned_at NULL; missing: orphaned_at set; changed: description/test_contract updated in place, row id preserved, group_rules grants preserved. _**Closed in 0.6.0.N session 4 (new-rule sub-case) and session 9 (missing + changed sub-cases) — see CHANGELOG entries.**_ |
| X.08 | Upgrade atomic swap — in-flight capability call completes within drain window | Old handler serves the in-flight call; new handler serves next call; swap completes cleanly. _**Closed in 0.6.0.N session 9** via `InflightSimulator` (worker-thread `DispatchGuard` over the production drain mechanism); see CHANGELOG `2026-04-29 — 0.6.0.N test-fixture buildout, session 9 of N`._ |
| X.09 | Upgrade atomic swap — drain timeout exceeded | New row → INSTALL_FAILED with kind `upgrade-drain-timeout`; old row → ACTIVE (untouched); 504 response. _**Closed in 0.6.0.N session 9** with one production deviation: HTTP 400 (not the ICD's 504) because `install_lifecycle.cpp:1402` hardcodes `failed_at = UPLOADING` for the upgrade-path conversion — same root cause as session 4's X.06 deviation; both reconcile together as the failure-conversion follow-up._ |
| X.10 | Upgrade asset routes — old 404, new serves | `/ext/notes/1.2.3/main.js` → 404 immediately post-swap; `/ext/notes/1.3.0/main.js` → 200 |
| X.11 | Upgrade filesystem — both versions coexist | `{data_dir}/extensions/notes/1.2.3/` present; `{data_dir}/extensions/notes/1.3.0/` present; `active` symlink → `1.3.0` |
| X.12 | Crash at swap T3 (after old → SUPERSEDED, before new → ACTIVE) | Reconciler detects two rows for same name (SUPERSEDED + ACTIVATING with `supersedes_id`); replays swap to final state; symlink re-checked and corrected |
| X.13 | Concurrent POSTs for the same name with different new versions | First acquires advisory lock, proceeds; second waits (up to a configurable bound) or 409 `in-flight-operation` |

### GC Contract (G.*)

These are unit-level tests in a separate file (`lifecycle_transitions_unit_test.cpp`); PG-gated only for the test DB fixture seeding, not for core logic.

| # | Case | Expected |
|---|---|---|
| G.01 | `garbage_collect_superseded_versions(24h)` identifies only SUPERSEDED rows with `retired_at < NOW() - 24h` | Returned `collected_ids` contains only eligible rows; `skipped_ids` empty |
| G.02 | GC run deletes row + filesystem tree | Row gone; version subdirectory gone; `active` symlink untouched; ACTIVE row for same name UNTOUCHED; `supersedes_id` on ACTIVE row → NULL via FK |
| G.03 | GC skips advisory-locked row | `collected_ids` excludes the lock-held row; `skipped_ids` contains its id |

### Unit-level (non-PG)

File: `tests/kernel/packages/lifecycle_transitions_unit_test.cpp`

- SemVer comparator round-trip (valid + invalid inputs; 0.4.1's parser is already under test but upgrade's comparator is additional).
- RBAC reconciliation comparator (v1_rules, v2_rules → RbacReconciliation) — pure set diff logic, PG-independent.
- Atomic-swap drain-state-machine (no PG, no Drogon; a mock in-flight-call counter) — verifies timeout semantics, completion semantics, zero-in-flight immediate completion.
- GC eligibility predicate: a pure function `is_eligible(retired_at, now, retention_hours) -> bool`. Edge cases at the boundary.
- State-enum string conversions round-trip for the added `SUPERSEDED` value.

**Test count: 9 D.* + 7 U.* + 13 X.* + 3 G.* = 32 fixture cases + unit-level file. I.11 / I.12-analog crash-recovery cases (U.06, U.07, X.12) use the 0.4.4 slice B seeded-state approach — per CHANGELOG v0.4.4 deviation note: `No fork()/exec()/SIGKILL subprocess harness. Crash recovery exercised by seeding plinth.packages rows with manufactured in-flight states + invoking reconcile_in_flight_installs() in-process.` Apply the same pattern to U.06/U.07/X.12.**

---

## CI Wiring

- `migrations/schema.sql` — three edits described in §Data Model (state CHECK + `retired_at` + `supersedes_id` + one partial index for `supersedes_id`).
- `src/kernel/packages/install_lifecycle.hpp` — extended with four entry points + GC contract function + drain config.
- `src/kernel/packages/install_lifecycle.cpp` — extended with the four bodies + swap choreography + GC body + reconciler extensions.
- `src/kernel/packages/handlers.{hpp,cpp}` — **new controllers** for `PATCH /api/packages/{id}` and `DELETE /api/packages/{id}`. `POST /api/packages` handler extended to route into upgrade branch.
- `src/kernel/packages/asset_server.{hpp,cpp}` — no API change. Existing `register_routes` / `unregister_routes` reused.
- `src/kernel/rbac/rule_registrar.{hpp,cpp}` — **new helpers** `mark_extension_rules_orphaned`, `clear_extension_rules_orphaned`, `delete_extension_rules`.
- `src/kernel/capabilities/resolution.{hpp,cpp}` — **new function** `unregister_capability`.
- `src/kernel/packages/migrations.{hpp,cpp}` — no change (0.4.3's `drop_schema_and_migrations` consumed as-is).
- `tests/fixtures/lifecycle_transitions/` — 25 new fixtures (see §Test Cases).
- `tests/kernel/packages/lifecycle_transitions_test.cpp` — **new**. PG-gated. Drives D.* + U.* + X.* cases through the full library surface.
- `tests/kernel/packages/lifecycle_transitions_unit_test.cpp` — **new**. No PG gate. Unit cases for GC predicate, RBAC comparator, SemVer compare, drain state machine, enum round-trip.
- `tests/kernel/packages/atomic_swap_crash_test.cpp` — **new**. PG-gated. Seeded-state crash-recovery cases for X.12 + U.06/U.07.
- `src/kernel/main.cpp` — adds the `{data_dir}` single-mountpoint check to the bootstrap self-check list (§Security Constraint 4). No change to the reconciler invocation site — same function, extended body.
- `CMakeLists.txt` — no new external dependencies. libzip and shell_blob from 0.4.4 are untouched. New test TUs added to the existing test-source globs.
- `run-clang-tidy-20` zero findings on new + modified TUs.
- CI: integration tests add ~30s for D.*+U.*+X.* PG-gated cases; unit cases add <1s. Crash-recovery cases add ~10s (no subprocess fork; all in-process seeded-state per 0.4.4 slice B pattern).

---

## Entry / Exit

**Entry:** ICD-0.4.4 merged and tagged; v0.4.4.1 (WS teardown test-strategy redesign) merged per RE-EVAL-0.4.x-following-0.4.4 §2.2; capability registry + RBAC enforcement + audit log + QuickJS runtime pool all operational (unchanged from 0.4.4 entry). No new external dependencies.

**Exit:** All D.* + U.* + X.* + G.* cases green under `PLINTH_KERNEL_TESTS=ON`; unit cases green under default `ctest`; `run-clang-tidy-20` clean on new TUs; `PATCH /api/packages/{id}` manually verified with `curl` (disable + enable cycle on a test package); `DELETE /api/packages/{id}?confirm=true` manually verified (package fully removed); upgrade manually verified (v1 → v2 via `curl -F package=@v2.zip`; old URL 404s; new URL serves); schema migration landed at `migrations/schema.sql` with `SUPERSEDED` + `retired_at` + `supersedes_id`; CHANGELOG entry describes the four transitions, the atomic-swap choreography, and the GC contract; ROADMAP `0.4.5` line removed; DEFERRED.md amended if any implementation-time deferral surfaces (none anticipated — the I.18/I.19/I.20 entry stays orthogonal).

---

## Open Questions (resolve during implementation)

1. **Default `upgrade_drain_timeout_ms`.** 5000ms proposed. Empirical tuning during implementation or early-0.4.5-ship telemetry may shift this. Alternative: poll once-per-100ms with a total budget, which gives cheaper cancellation granularity; proposed approach does a single timed wait on a condition variable. Architect preference at PR time.

2. **Per-step uninstall audit events vs. terminal-only.** Mirrors ICD-0.4.4 Open Question #5. Proposed: per-step (`packages.uninstall_routes_unregistered`, `packages.uninstall_rules_deleted`, `packages.uninstall_schema_dropped`, `packages.uninstall_files_removed`) for forensic clarity. Alternative: single `packages.uninstall_confirmed` + `packages.uninstalled` with structured detail. ~5 rows per uninstall is negligible; forensic value is non-trivial. Architect preference.

3. **`supersedes_id` FK action.** `ON DELETE SET NULL` proposed. Alternative: `ON DELETE CASCADE` (deleting the old row cascade-deletes the new row — definitely wrong), or `ON DELETE RESTRICT` (can't delete the old row while the new references it — requires explicit two-step uninstall when both versions are in SUPERSEDED, unlikely but possible edge). SET NULL keeps the reconciler's life simple. Architect confirmation.

---

## Appendix A: Example Upgrade Timeline

Concrete walk-through for the hypothetical Notes extension upgrading from v1.2.3 to v1.3.0, anchoring the atomic-swap choreography:

```
(Precondition: notes v1.2.3 is ACTIVE. 3 RBAC rules: notes.edit, notes.render, notes.share.
  2 capabilities: notes:1:edit, notes:1:render. 1 panel: editor.
  v1.3.0 package zip adds a new capability notes:1:comment + its rule notes.comment,
  and drops notes.share.)

t=0.000s  Admin: curl -X POST https://plinth.example.com/api/packages \
                     -H "Authorization: Bearer $PAT" \
                     -F "package=@notes-1.3.0.zip"
t=0.020s  RbacFilter: packages.install rule present. Forward to handler.
t=0.021s  UPLOADING: stream zip (4.8 MB) to staging.
t=0.085s  Magic-number OK. libzip extracts. Path-traversal check passes.
t=0.115s  Minimal manifest: name="notes", version="1.3.0". Advisory lock on notes acquired.
t=0.118s  Collision check: SELECT id, version, state FROM plinth.packages
             WHERE name='notes' AND state IN (...) AND uninstalling_at IS NULL
             → row{id=3c7e8b1e-..., version=1.2.3, state=ACTIVE}.
          SemVer compare: 1.3.0 > 1.2.3 → upgrade branch.
t=0.120s  INSERT plinth.packages (state='UPLOADING', version='1.3.0',
             supersedes_id='3c7e8b1e-...') → id=7fe13d...
t=0.121s  Transition: new row state='VALIDATING'.

t=0.122s  VALIDATING: validate(package_root, {cross_file=true, against_running_kernel=true,
             in_process_registry=true, upgrade_from_id='3c7e8b1e-...'})
             Layer 1 GlassWorm → clean
             R1..R6, CF1..CF7, CFW1..CFW4 → pass
             RT1 (name collision) → skipped for upgrade_from_id match
             RT2 (mount collision) → N/A (no frontend.mount)
             RT3 (requires resolution) → notes:1:comment not yet registered; self-provides OK
t=0.180s  ValidationReport{disposition=0}. Transition: new row state='MIGRATING'.

t=0.181s  MIGRATING: run_migrations("notes", package_root, admin_conn)
             Existing ext_notes schema: 001_create_notes.sql, 002_add_tags.sql applied.
             New migration 003_add_comments.sql present in zip.
             003 applies; plinth.migrations row inserted.
t=0.260s  MigrationReport{applied: [003], skipped: [001, 002], warnings: []}.
          Transition: new row state='REGISTERING'.

t=0.261s  REGISTERING: BEGIN
             UPDATE plinth.packages (new row): state='REGISTERING', full manifest, etc.
             register_capability("notes:1:edit", ...)  -- idempotent upsert; old registry entry
                                                          still serves dispatch
             register_capability("notes:1:render", ...) -- idempotent
             register_capability("notes:1:comment", ...) -- new
             RBAC reconciliation:
               V1: {notes.edit, notes.render, notes.share}
               V2: {notes.edit, notes.render, notes.comment}
               → INSERT notes.comment
               → UPDATE notes.edit in place (desc unchanged; orphaned_at=NULL idempotent)
               → UPDATE notes.render in place (idempotent)
               → SET orphaned_at=NOW() on notes.share
             register_panel(new_package_id, "editor", PRIMARY, ...)
             UPDATE new row: state='EXTRACTING'. COMMIT.
             LISTEN/NOTIFY fires; capability caches refresh.

t=0.380s  EXTRACTING: copy staged files to /var/lib/plinth/extensions/notes/1.3.0/
             fsync every file.
             Staging unlinked.
             (active symlink still points at 1.2.3 — NOT touched here.)
t=0.460s  Transition: new row state='ACTIVATING'. --- ATOMIC SWAP BEGINS ---

t=0.461s  T0: intent declared (state='ACTIVATING', supersedes_id already set).
t=0.462s  T1: drain window opens on notes:1:* dispatch for version 1.2.3.
             At T1+0ms: 2 in-flight notes:1:edit calls observed by the drain counter.
t=0.478s  T2: in-flight count drops to 0 at T1+16ms. Drain completes.
t=0.479s  T3: BEGIN
               UPDATE plinth.packages (old row): state='SUPERSEDED', retired_at=NOW().
               UPDATE plinth.packages (new row): state='ACTIVE'.
             COMMIT.
             symlink rename: /var/lib/plinth/extensions/notes/active → 1.3.0
t=0.480s  T4: asset_server::unregister_routes("notes", "1.2.3")
                 /ext/notes/1.2.3/* → 404 effective immediately
             asset_server::register_routes("notes", "1.3.0", ...)
             unregister_capability("notes:1:edit", ..., extension_id=old)  (no-op on LISTEN/NOTIFY
                                                                            for edit — it's still
                                                                            in registry for new id)
             unregister_capability for any capability present in v1 but not v2 — none in this example
t=0.481s  T5: retained — old row lives in SUPERSEDED until GC.
t=0.482s  ACTIVATING tail: audit packages.upgrade_swapped event.
t=0.483s  Audit: packages.upgrade_completed{name=notes, from=1.2.3, to=1.3.0,
             rbac_reconciliation={added:[notes.comment], updated:[notes.edit, notes.render],
                                  orphaned:[notes.share]}, drain_waited_ms=16}
t=0.484s  Release advisory lock.

t=0.485s  HTTP 201 to admin:
             {
               "id": "7fe13d...-new",
               "name": "notes",
               "version": "1.3.0",
               "state": "ACTIVE",
               "supersedes": {
                 "id": "3c7e8b1e-...-old",
                 "version": "1.2.3",
                 "retired_at": "2026-05-01T12:00:00.479Z"
               },
               "rbac_reconciliation": {
                 "added": ["notes.comment"],
                 "updated": ["notes.edit", "notes.render"],
                 "orphaned": ["notes.share"]
               },
               "warnings": []
             }

t+~5s     User browser: GET /ext/notes/1.2.3/main.js (stale URL from cached shell)
             asset_server: map lookup (name=notes, version=1.2.3) → miss (unregistered at T4) → 404
             Browser: cache invalidated.
t+~5.1s   User browser: GET /ext/notes/1.3.0/main.js
             asset_server: map lookup (name=notes, version=1.3.0) → hit → 200 + immutable
             cache headers.

t+24h     0.7.x scheduler runs garbage_collect_superseded_versions(24h)
             Eligible: old row id=3c7e8b1e-..., retired_at=t-24h-ε → collect.
             Advisory lock acquired non-blocking → success.
             DELETE FROM plinth.packages WHERE id='3c7e8b1e-...'
             remove_all("/var/lib/plinth/extensions/notes/1.2.3/")
             `active` symlink untouched (still → 1.3.0).
             FK: supersedes_id on new row → NULL.
             GcReport{collected_ids=['3c7e8b1e-...'], bytes_freed=4_800_000}.
```

**Disable timeline:**

```
t=0.000s  Admin: curl -X PATCH https://plinth.example.com/api/packages/7fe13d... \
                     -d '{"action": "disable"}'
t=0.020s  RbacFilter + JSON body parse.
t=0.021s  Advisory lock on notes acquired. BEGIN.
t=0.022s  SELECT ... FOR UPDATE → state=ACTIVE.
t=0.023s  UPDATE plinth.packages SET state='DISABLED', disabled_at=NOW() WHERE id=...
t=0.024s  mark_extension_rules_orphaned("notes", conn) → 3 rows affected (notes.edit,
             notes.render, notes.comment — the post-upgrade set).
             unregister_capability("notes:1:edit"), ..., "notes:1:comment")
t=0.026s  asset_server::unregister_routes("notes", "1.3.0")
t=0.027s  COMMIT.
t=0.028s  Audit: packages.disabled{name=notes, version=1.3.0, disabled_by_user_id=...}.
          Release lock.
t=0.029s  HTTP 200 with disabled record body.
```

**Uninstall timeline:**

```
t=0.000s  Admin: curl -X DELETE \
             https://plinth.example.com/api/packages/7fe13d...?confirm=true
t=0.020s  RbacFilter + confirm=true check.
t=0.021s  Advisory lock on notes acquired.
t=0.022s  Tx A: UPDATE state='UNINSTALLING', uninstalling_at=NOW(). COMMIT.
t=0.023s  Audit: packages.uninstall_confirmed{...}.
t=0.024s  asset_server::unregister_routes("notes", "1.3.0")
             unregister_capability (3 capabilities)
t=0.028s  Tx B: BEGIN
             DELETE FROM plinth.group_rules WHERE rule_id IN (SELECT id FROM plinth.rbac_rules
                 WHERE extension_name='notes')
             DELETE FROM plinth.rbac_rules WHERE extension_name='notes'
             DELETE FROM plinth.panels WHERE package_id='7fe13d...'
             DELETE FROM plinth.capabilities WHERE extension_name='notes'
          COMMIT.
t=0.050s  drop_schema_and_migrations("notes", admin_conn)
             DROP OWNED BY ext_notes_role CASCADE
             DROP ROLE ext_notes_role
             DROP SCHEMA ext_notes CASCADE
             DELETE FROM plinth.migrations WHERE extension_name='notes'
t=0.190s  remove_all("/var/lib/plinth/extensions/notes/")
t=0.220s  Tx C: DELETE FROM plinth.packages WHERE id='7fe13d...'. COMMIT.
t=0.221s  Audit: packages.uninstalled{name=notes, version=1.3.0, prior_state='ACTIVE'}.
          Release lock.
t=0.222s  HTTP 204.
```
