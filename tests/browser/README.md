# Shell browser smoke

Install with `npm ci --prefix tests/browser` and
`cd tests/browser && npx playwright install --with-deps chromium`.
Run `npm test --prefix tests/browser` to package the actual shell ZIP, extract
it unchanged, and serve its files under the production CSP. Set
`PLINTH_SHELL_ZIP=/absolute/path/to/shell.zip` to test an existing build artifact.

For the production gate, start a task-owned kernel with a disposable database
and a fresh shell installation, then run:

```sh
PLINTH_BASE_URL=http://127.0.0.1:8080 npm test --prefix tests/browser
```

This mode obtains the document, assets, redirect, CSP and initial session
response from the running kernel. It does not rewrite production assets.
The demo panel file, demo snapshot response, WebSocket transport, and boundary
audit sink are controlled by the browser harness. Authentication, realtime
protocol integration, and audit persistence require their separate server
suites; this gate proves startup and module/hook execution.

Every entry-point check uses a fresh browser context. Failures include script,
stylesheet, or font HTTP/network errors, uncaught exceptions, CSP violations, a
missing sign-in screen, broken documented/versioned SDK imports, and failed
rendering of the maintained SDK demo with both Preact and SDK hooks. The
boundary test verifies stack omission in the shipped configuration and stack
inclusion with an explicit development configuration.

`PLINTH_BROWSER` selects an existing Chromium-compatible executable. When
required by a container that cannot run the Chromium sandbox,
`PLINTH_BROWSER_NO_SANDBOX=1` explicitly disables it.
