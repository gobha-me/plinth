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

Validate a package before installation:

```bash
./build/plinth validate path/to/extension
./build/plinth validate path/to/extension --json
```

The validator enforces manifest structure, cross-file references, file and
package bounds, migration naming and SQL policy, capability signatures, RBAC
shape, and Unicode-smuggling defenses. Never bypass a validation error by
weakening the package or kernel checks.

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
