# Extension guide

Plinth extensions are declarative packages whose manifests describe identity,
capabilities, RBAC rules, migrations, optional frontend panels, and QuickJS
handlers. The kernel owns authentication, authorization, storage boundaries,
package lifecycle, and dispatch.

The test fixture at `tests/extensions/sdk-demo/` is the smallest maintained
example. It is not bundled into production data and must not be treated as an
installed application.

## Package shape

An extension begins with `manifest.json`. Depending on its features it can also
contain:

- `capabilities.json` for exported capability signatures;
- `rbac.json` for rules and default grants;
- `migrations/` for extension-schema migrations;
- `server/handlers/` for QuickJS handlers;
- `panels.json` and `client/` for browser panels;
- `config.json` for extension-owned configuration schema.

## Static validation

Validate an unpacked package directory before installation:

```bash
./build/plinth validate path/to/extension
./build/plinth validate path/to/extension --json
./build/plinth validate path/to/extension --structure-only
```

The default pass shares manifest structure, file and package bounds,
Unicode-smuggling defenses, cross-file references, typed RBAC validation and
static migration discovery with install and upgrade. `--structure-only` keeps
structure/schema parsing and the source scan, skips cross-file semantics and
migration discovery, and preserves the missing-handler warning instead of the
default cross-file error. Never bypass a validation error by weakening the
package or kernel checks.

Typed `rbac.json` parsing reports schema errors in both modes. The default pass
also checks rule names, package/reserved namespaces, and both
`test.assert_deny.call` and `test.assert_allow.call` against capabilities
provided by the package. These use the same DB-free A.1-A.4 checks as
registration. Structural RBAC errors suppress its typed semantic checks so
reported array indexes remain correct; other files and legacy cross-file
checks still report. Existing legacy `test.call` diagnostics remain available.
Registration separately rechecks rule ownership in PostgreSQL (A.5).

Static migration validation reuses the migration library's discovery pass:
ordinary filenames, numeric sequence ordering, duplicate sequence rejection,
qualified-DDL policy, and nonfatal sequence-gap warnings. An absent migration
directory is valid. Discovery retains its first-failure behavior; it does not
execute SQL or prove that migrations will apply to a target database. Its
new validation rule codes are:

| Rule | Disposition |
|---|---|
| `migration-duplicate-sequence` | Error |
| `migration-invalid-filename` | Error |
| `migration-unqualified-ddl` | Error |
| `migration-read-failed` | Error |
| `migration-gap` | Warning |

Typed RBAC A.1-A.4 checks and migration discovery run after the existing CF/CFW
passes, in that order. Existing diagnostic codes and relative pass ordering
remain unchanged. A static RBAC or migration defect now fails installation at
`VALIDATING` with `validation-errors`, before migration application or
registration. Database-dependent and execution failures retain their later
stages.

The default directory-size limit is 50 MiB (52,428,800 bytes);
`--max-size <bytes>` sets a local explicit limit. Exact equality is allowed.
Install and upgrade use the operator's effective package byte limit for that
same static check. ZIP upload size and extraction admission remain separate
checks with their existing bounds; a directory pass does not certify a ZIP.
The CLI does not load a server configuration or discover a target's policy.

The exit code is 0 for no findings, 1 for errors and 2 for warnings only.
Warnings permit installation. `--quiet` suppresses text; `--quiet --json`
still emits JSON. JSON is one newline-terminated object on stdout with `path`
(absolute), `exit_code`, `files_scanned`, `total_bytes` and `messages`.
Each message has `severity`, `phase` (`structure`, `cross_file` or
`runtime_state`), `rule`, nullable `path`, `message` and nullable `remediation`.
CLI usage errors retain stderr output and exit 1. Installed reports retain
their separate `disposition` wrapper and optional-field representation.

Within a pass, filesystem findings use lexical generic relative-path order;
manifest arrays retain source order. For an unchanged readable tree at the
same path with the same effective policy, repeated reports are deterministic.
Absolute root metadata and filesystem error prose can differ between paths
or environments. The source scanner retains its own per-pass scope and may
report a file more than once when it is also the declared entry point.

Validation is static pre-install feedback. The existing
`--against-running-kernel` mode remains unimplemented; it cannot establish
target-state parity. Dependency availability, installed-name/mount conflicts,
database ownership/checksums, SQL execution and runtime readiness still need
their target-dependent checks. Issue [#84](https://github.com/gobha-me/plinth/issues/84)
remains open for that broader validation contract.

## Security boundaries

- Extension SQL executes in an extension-owned schema with an enforced search
  path.
- Capability calls pass through kernel authorization; browser code does not
  receive direct database or audit-log authority.
- QuickJS runtime limits bound memory, execution time, stack depth, async work,
  and database batching.
- Package activation, replacement, draining, and recovery are coordinated by
  the kernel lifecycle.
- Static assets are served from the installed package root after path and
  manifest validation.

Start with [extension architecture](architecture/05-extensions.md), then use the
versioned contracts in `docs/icd/` for the subsystem being implemented. The
architecture documents are normative when older design sketches or changelog
entries disagree.

## Testing

Place hermetic package fixtures under `tests/fixtures/` and behavior tests under
`tests/kernel/packages/` or the owning subsystem. Cover malformed and hostile
inputs before the happy path. Runtime-state tests need a disposable PostgreSQL
database and must clean only their task-owned schemas and artifacts.
