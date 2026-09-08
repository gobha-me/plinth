# Upgrade the bundled shell on a retained installation

Replacing the configured `shell.zip` or installing a newer kernel binary does
not automatically upgrade an installed shell. The bundle path is a trusted
operator configuration; HTTP uploads cannot acquire bundled provenance or
replace the reserved `shell` package.

1. Keep a backup of the database and package data directory together. Stop the
   kernel; use the same database, data directory and configuration throughout.
2. Install the new trusted `shell.zip` at the configured `shell.bundle_path`
   directory, and inspect `plinth shell status --config config.json` (add
   `--json` for automation). Status reports installed and available manifest
   versions and whether an upgrade is available. It performs bounded archive
   inspection and database reads only; it does not bootstrap or migrate.
3. With `dev_mode=false`, start
   `plinth serve --config config.json --upgrade-bundled-shell`. The explicit
   upgrade finishes before HTTP ingress. A newer manifest version is required;
   the same installed version is a no-op, and downgrades are refused.
4. On subsequent starts use the ordinary `plinth serve --config config.json`.
   The installed shell remains selected. An ordinary browser navigation
   revalidates mutable `/app/` assets; `/ext/shell/<version>/` stays immutable.

The operation retains preferences, existing RBAC rule identities and grants,
unchanged migration checksums/timestamps, and package supersedes relationships.
The new package retains BUNDLED provenance. Pending bundled migrations,
registration and activation share one database transaction; a later failing
migration or extraction rolls back earlier new files' schema/data changes and
migration tracking. Pending migrations cannot contain transaction-control SQL.
This transaction mode does not change ordinary USER package behavior.

On a validation or migration failure, the old installation remains usable.
Correct the trusted bundle and retry the same command at the same version.
An eligible INSTALL_FAILED candidate is reused only when its bundled provenance
and predecessor match the current installation; failure audit records remain.
No existing version directory is overwritten or removed by retry. An extraction
collision requires operator inspection of the conflicting directory before a
retry; the command preserves its files. Do not reset a populated database or
turn on `dev_mode` to resolve an upgrade error.

PostgreSQL commit and filesystem symlink rename cannot be one crash-atomic
operation. Ordinary errors restore the old pointer and roll back the database;
an unexpected process/power failure in that narrow interval can leave the
pointer inconsistent. Startup refuses a bundled frontend whose `active`
symlink does not match its committed installed version. While stopped, inspect
`plinth shell status`, the package rows and version directories, restore the
symlink to the database's committed version, and restart. Preserve both version
directories for inspection. If a failed COMMIT cannot restore the pointer, the
command reports `upgrade-recovery-required` and retains the new files. A lost
connection during COMMIT also requires checking the committed database outcome
before deciding which version to restore.

This workflow upgrades shell extension migrations only. The separate kernel
schema migration policy remains the milestone 0.7 gate; it does not make a
kernel requiring incompatible schema changes safe to run on a retained schema.

The acceptance command is
`python3 tests/browser/run-bundled-upgrade.py --binary /path/to/plinth` with the
usual `PLINTH_PG_*` test variables. It creates and removes its own database and
data directory, packages two real frontends, seeds retained preferences/grants,
checks failure rollback and same-version retry, and navigates both versions
using one persistent Chromium profile. It requires the browser harness and
Playwright installation documented in `tests/browser/README.md`.
