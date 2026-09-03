# sdk-demo — ICD-0.6.3 manual FE smoke fixture

**Posture:** test-only. Not bundled in `shell.zip`; not installed at first-boot.
The architect-pinned OQ4 resolution at the ICD-0.6.3 paper session is
"test-only fixture at `tests/extensions/sdk-demo/`" — production users
see no demo panel.

**Use:** load this extension via the real `POST /api/packages` install
path during manual FE smoke. The shell's `loadPanel` (in
`client/shell/client/panels/loader.js`) takes `(extName, extVersion,
panelId, container, opts)` and the fixture's panel ID is `demo`.

## Smoke walkthrough (per ICD-0.6.3 §13.5)

1. Reset PG schema; `plinth serve --dev`. First-boot installs the
   bundled shell (v0.6.3) automatically.
2. Build the fixture zip and POST it to `/api/packages` as the admin
   user.
3. Browse `http://localhost:8080/app`. Sign in as admin.
4. Open devtools → Console. Trigger the panel manually:

   ```js
   import('/ext/shell/0.6.3/client/panels/loader.js').then((m) =>
     m.loadPanel('sdk-demo', '0.1.0', 'demo',
                 document.querySelector('main'), {}));
   ```

5. Verify:
   - `[sdk_demo] onActivate` console log fires.
   - The panel renders with `theme: <value>` (proves
     `plinth.call('shell.preferences.get', ...)` round-trip).
   - From a side terminal:
     `plinth call pubsub.publish 'sdk_demo:test' '{"hello":"world"}'`
     → the "envelopes received" counter increments (proves
     `plinth.subscribe` round-trip).
   - Press `Ctrl+Shift+D` → console log proves
     `registerShortcut` dispatch.
   - Click "Trigger boundary throw" → boundary fallback UI renders;
     SQL `SELECT ... FROM plinth.audit_log WHERE
     action='ext.shell.frontend.boundary.caught'` shows a row with
     non-forgeable identity + sanitised detail keys.

## Why version 0.1.0 (not 0.6.3)

The fixture is independent of plinth's milestone arc. Bumping its
version is unnecessary for v0.6.3. Future fixture versions are bumped
when the SDK contract requires it.

## Implementation deviations per §17

1. **Name `sdk-demo` (not `sdk_demo` per ICD §D.1).** The manifest
   `name` regex `^[a-z][a-z0-9-]{1,63}$` rejects underscores; switched
   to dash. The shell's loader accepts the dashed name verbatim;
   panels.json + capabilities.json shapes are unchanged.

2. **`frontend` block omitted entirely.** ICD §D specified
   `frontend: { mount: null, entry: null }`. v0.6.1's `parse_manifest`
   only accepts a `frontend` block with non-null mount + entry strings;
   nullable shape isn't supported. Omitting `frontend` validates
   cleanly and matches the "primary panel only" use case.

3. **panels.json field is `client_path` (not `component` per ICD §A.5).**
   The v0.4.4 panels-manifest parser uses `client_path`; ICD-0.6.3 §A.5
   showed `component`. Both name the same path — the panel module
   relative to the extension root. The shell's loader was updated to
   read `panel.client_path` so this stays consistent across the parser
   and the loader.
