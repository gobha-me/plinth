# Shell smoke tests (manual)

ICD-0.6.0 §13 documents test cases T.\*, E.\*, I.\* (and L.\* per
deferral architect-signed-off this session). No headless-browser
harness ships in 0.6.0 per OQ2; the case files in this directory are
**documented manual smoke walkthroughs**, not automated tests. They
are pre-positioned for the **0.6.0.N test-fixture buildout** (ROADMAP
§0.x cleanup follow-ups) which absorbs:

- The headless-browser harness (Playwright + Chromium, or equivalent).
- The HTTP test fixture for L.\* wire-contract verification.
- The 25-case backfill across ICD-0.4.4 / 0.4.5 / 0.5.0.3 / 0.5.4 / 0.5.5.

Until the harness lands, run these walkthroughs manually before
shipping a 0.6.x change that touches the shell.

## How to run

1. `./build/plinth serve --config dev.toml` (or whatever your local
   dev invocation is) against a fresh-DB target.
2. Seed an admin user via the bootstrap path (e.g.
   `./build/plinth seed-admin --username admin --password ...`) if the
   target DB is not pre-seeded.
3. Open the relevant `*.html` file in this directory in a Chromium-
   based browser (DevTools open) and follow the steps in its `<ol>`.
4. Each `<li>` is one assertion. Watch for the listed expected outcome
   in the DOM, network panel, or console.
5. Any failure → file a bug; do NOT mark the case green.

## Files

- [topbar_render_test.html](topbar_render_test.html) — T.01–T.05: four-zone topbar render after auth
- [boundary_render_test.html](boundary_render_test.html) — E.01–E.02: top-level Preact error boundary
- [login_walkthrough_test.html](login_walkthrough_test.html) — I.01–I.02: end-to-end navigate + login + sign-out

## Deferred to 0.6.0.N

- **L.01–L.06** — login wire-contract (`/api/auth/login`,
  `/api/auth/session`, `/api/auth/logout`); needs an HTTP test fixture
  the 0.6.0.N buildout will land. Coverage today: SQL layer via
  `tests/kernel/auth/auth_integration_test.cpp`; 0.6.0 manual smoke
  via `login_walkthrough_test.html`.
- **B.06** — handler-ordering integration assertion that
  `/api/auth/session` is not shadowed by `/app/{path:.*}`. Verified
  manually via the walkthrough; structurally enforced by
  `register_shell_routes` slotting AFTER `asset_server::restore_routes`
  at `src/kernel/main.cpp`.
- **T.\*/E.\*/I.\*** — browser-driven assertions; need the headless
  harness.
