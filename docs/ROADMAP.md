# Plinth Roadmap

Version scheme: `0.<milestone>.<task>`
Tag per X.Y.Z task completion. Four-part follow-ups (X.Y.Z.N) accumulate
into the next X.Y.Z tag range; they ship and land on main, but do not
carry their own tag. Squash on merge to main. Completed milestones are
removed (see CHANGELOG.md for history).

Schema rules: 0.1–0.6 schema is fluid (edit schema.sql directly).
0.7+ schema is frozen (numbered immutable migrations only).
See SESSION-GUIDE.md for details.

## Band labels and re-evaluation

Every pending milestone carries a band label — `[strong]`, `[medium]`,
or `[fuzzy]` — per the commitment gradient in
`METHODOLOGY-llm-assisted-development.md` §Phase 0 *Roadmap Milestone
Labels*. Plinth uses the methodology's default gradient window
(N=3, M=7): strong = load-bearing for the next 3 milestones;
medium = directional for milestones 4–7 out; fuzzy = sketch for
anything further. Completed milestones are removed per the preamble
rule; see CHANGELOG.md for history.

Re-evaluation items appear interleaved with code milestones at a
cadence of **every 4 code milestones** (tentative — revisit at the
first re-eval). Each re-eval runs both the paper half (slide bands,
update labels, relocate aged content) and the code-aware half (read
the code against the architecture, produce a gap analysis) per
METHODOLOGY §Phase 3 — see §3.1.1 *Code-Aware Inputs* and §3.3
*Scheduling*. Each re-eval also performs a **forward ICD presence
check** on pending `[strong]` milestones in the next-N window per
METHODOLOGY §3.1; a missing ICD triggers either a demotion or a
scheduled `X.Y.Z.N` ICD-authoring slot ahead of the milestone.
Re-eval items carry the `[rewrite session]` tag rather than a band
label. No further code milestone begins until the preceding re-eval
item completes.

The parallel 0.6a admin-extension stream and the 0.6b
additional-bundled-extensions stream do not count toward the re-eval
cadence (gated items, not in strict sequence).

## 0.x cleanup follow-ups (cross-cutting; scheduled between milestones)

These items accumulated as deliberate ship-time deferrals across
the 0.4.x and 0.5.x arcs. None blocks any in-flight code milestone;
each lands as a dedicated `X.Y.Z.N` follow-up at the indicated
schedule slot. Source-of-truth entries live in `docs/DEFERRED.md`;
this section is the scheduling ledger.

- [ ] **0.6.0.N — Test-fixture buildout (HTTP + advisory-lock + live-buffer fault-injection)**
  One-shot fixture investment that retires the cross-arc
  test-coverage debt accumulated since v0.4.4. Three composable
  fixtures land together because they share scaffolding shape:
  Drogon-with-RBAC-seeded-session HTTP fixture, multi-process
  advisory-lock harness, and a live-buffer fault-injection seam on
  `WsTestClient`. Closes ~25 deferred ICD test cases:
  ICD-0.4.4 I.18 / I.19 / I.20 (HTTP POST concurrency + dry_run +
  RBAC denial); ICD-0.4.5 X.05 / X.06 / X.07 / X.08 / X.09 / X.10 /
  X.11 / X.12 / X.13 + G.03 (extended upgrade + GC coverage —
  X.12 crash-injection needs a separate fork/SIGKILL decision);
  ICD-0.5.0.3 R.02 / R.03 / E.07 / P.01 / P.04 / P.05 / H.02 /
  H.03 / C.01 / C.02 (extension-dispatch coverage);
  ICD-0.5.4 I.02 + I.03 (multi-process advisory-lock + live-replay
  race in integration); ICD-0.5.5 S.07 + I.01 / I.02 / I.03 / I.04
  (multi-node failover + cross-extension ordering integration). S.06
  broker gap audit closed in session 8; L.03 / L.04 / L.05 mid-replay
  buffering closed in session 6. X.07 missing+changed / X.08 / X.09
  drain-window closed in session 9. Only ICD-0.4.5 X.12 + ICD-0.5.5
  S.07 SIGKILL family remain in this milestone arc. Build at
  `tests/kernel/packages/http_test_fixture.{hpp,cpp}` (HTTP arm),
  `tests/util/advisory_lock_harness.{hpp,cpp}` (multi-process arm),
  and `tests/util/ws_test_client.{hpp,cpp}` extension (buffer-cap
  override + drain-pause hook). Schedule between 0.6.0 ship and
  0.6.1 start; can ship in parallel with `0.6.0.N ICD-0.6.1
  authoring`. See `DEFERRED.md` 2026-04-20 / 2026-04-22 / 2026-04-23
  / 2026-04-26 entries.   [strong]
- [x] **0.5.x.N — `[js][async]` kernel-side refcount investigation**
  _Promoted at `RE-EVAL-0.5.x-following-0.5.5.md §2.4` (2026-04-26)
  to **`0.5.5.2 [js][async] refcount fix [strong]`** as a scheduled
  milestone blocking 0.6.0; full description now at ROADMAP §0.5.
  The `[medium]` cross-cutting framing no longer applies — the maintainer's
  "0.5.x is the kernel's last hardening window" directive moved this
  out of cross-cutting into the main arc._

## Load Harness (parallel test framework; gated on capability unlocks)

Standalone external binary — own process, own build, no link against
`plinth` or `plinth_tests`. Purpose: stress + performance testing
against the **production** `plinth` kernel (not the Catch2 subprocess
harness). Two immediate concerns: (1) saturate HTTP + WS + cap.call
chain-depth paths to observe real behavior under load; (2) empirically
audit the working hypothesis behind the 0.4.4.1 teardown redesign —
does the production kernel hit the same `bad_weak_ptr` family under
load, or is it test-harness-only?

Observability is external for now: `ps` / `top` / `perf stat` for
RSS + CPU, `SELECT state, count(*) FROM plinth.packages GROUP BY state`
for endstate checks, and log tailing for the known-bad teardown
signatures. No dependency on `plinth.metrics` (0.7.1 wires that in at
LH-4). Language + topology selected at LH-0 implementation time.

Each item gates on the kernel capability it exercises. Schedule can
slip — none of these block the main arc.

- [x] LH-0 Scaffold: HTTP auth + POST /api/packages to install a test extension + cap.call chain depth N from a driver extension; easy + medium tiers; external ps/top observability. Gated on 0.4.4 (shipped).   [strong]
   _Shipped 2026-04-21 (untagged). cap.call chain dispatches via the kernel Tier 1 `lh0:1:chain` recursion; async-bridge stress deferred to LH-0.1 per ICD-LH-0 §8._
- [x] LH-0.1 Async-bridge stress: new `lh0:1:js_stress(script)` kernel signature dispatched via a fork in `on_call` (not the Tier 1 map), driving `run_on_context` on a process-lifetime RuntimePool; new `--tier=async` in the harness. Exercises the `signal_completion → JS_ExecutePendingJob` path LH-0's sync recursion cannot reach. Gated on LH-0 (shipped).   [strong]
   _Shipped 2026-04-21 (untagged). ICD-LH-0.1 §9 mandates 3-trial repro or clean-run data as the diagnostic output._
- [ ] LH-1 LISTEN/NOTIFY subscribe + notify-storm tier. Gated on 0.5.0.   [medium]
- [x] LH-2 N subscribers × event-flood tier (WS fan-out saturation). Gated on 0.5.2.   [strong]
   _Shipped 2026-04-24 (untagged). `load-harness/cmd/lh0/` gained `--tier=ws-fanout` + `load-harness/internal/wssub/` + `load-harness/fixtures/lh2sidecar/` (sidecar arm scaffolded; first-ship WS-only per OQ1 redirect — see CHANGELOG §Implementation deviation 3). 3/3 trials clean against v0.5.2 HEAD + this branch's widened `classify_pubsub_subscribe`: 4 producers × 4 WS subscribers × 120 s × burst=8 = {262k, 207k, 170k} envelopes emitted with observed/emitted 1.0000 × 4 subs per trial, p99 WS lag < 100 ms, zero `free_zero_refcount` / `list_empty` / `bad_weak_ptr` / `realtime.broker.*` audit reproductions. Clears the broker for LH-3 / 0.5.4 delta-sync. Bundled kernel changes: `classify_pubsub_subscribe` widened for cross-ext per ICD-0.5.2 §SC6; PUBSUB_SUBSCRIBE dispatch-arm re-check per SC2; `RuntimePool::destroy` persistent-callbacks leak fix. 8 new `[js][realtime][broker][rbac]` test cases in `tests/kernel/js/pubsub_subscribe_test.cpp`._
- [ ] LH-3 Reconnect-under-storm tier (delta-sync stress). Gated on 0.5.4.   [medium]
- [ ] LH-4 Hard + crushing tiers consolidated; harness result rows wired into `plinth.metrics`. Cross-validation pass: compare `plinth.metrics` RSS/CPU values against the ps/top/perf measurements the harness already collects externally; `plinth.metrics` must agree within an acceptable % (threshold set at implementation time). Validates the metrics subsystem against an independent ground truth before it becomes the CI regression gate. Gated on 0.7.1.   [medium]

## 0.5 — Realtime
_All 0.5.x milestones shipped 2026-04-22 → 2026-04-27 (v0.5.0 → v0.5.5
+ 0.5.5.1, 0.5.5.2, RE-EVALs, ICD-authoring follow-ups). See
`docs/CHANGELOG.md` for per-tag entries — preamble rule removes
completed lines from this section._
## 0.6 — Frontend Shell
- [x] 0.6.0 Bootstrap and frame: Preact/htm scaffold, login wired to 0.1.x auth, empty topbar (home/app-name/tray/avatar zones), error boundary scaffolding   [strong]
   _Shipped 2026-04-27 (untagged per `feedback_tagging_rule.md`). Branch `feat/0.6.0-frontend-shell-bootstrap`. First UI code milestone. Five-phase commit arc landing: (1) `Config::Shell { enabled, root_redirect }` block + JSON loader with regex validation per ICD §9.2; (2) Preact 10.22.0 + htm 3.1.1 vendored under `client/shell/client/vendor/` (CSP-clean — no `eval(`/`new Function(` in either runtime); single-file `shell.js` with `plinthFetch` redirect-on-401 wrapper, controlled `LoginForm` with OQ3 countdown, `AuthFrame` with four-zone topbar + Sign Out popover, top-level Preact `Boundary` per ICD §7; manifest version bumped 0.1.0 → 0.6.0 and `frontend.mount` flipped `/` → `/app`; (3) `src/kernel/shell/static_handler.{hpp,cpp}` with `register_shell_routes(cfg, db, data_dir)` resolving the active bundled frontend from `plinth.packages` at boot (`provenance='bundled'` + `frontend_mount IS NOT NULL` + `state IN ('ACTIVE','ACTIVE_FLAGGED')`) and registering both `GET /` → 302 `cfg.root_redirect` and `GET /app/(.*)` SPA-fallback handler; serves files from `<data_dir>/extensions/<name>/<version>/client/` with strict CSP (`script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'`) and `no-cache`/`immutable` cache headers; B.01–B.05 + B.07 + B.08 + path-traversal hardening tests green via `test_seam::dispatch_app`; (4) manual smoke HTML scripts under `tests/shell/` for T.\*/E.\*/I.\* + L.\* per OQ2 deferral — full headless harness lands at `0.6.0.N Test-fixture buildout`; (5) docs promotion (this entry, `architecture/06-frontend.md §1` footnote, `architecture/05-extensions.md §2` reserved-prefix table flips for `/app/*` and `/`). **OQ1 pin: bundle byte source = on-disk installed shell** (architect override of ICD §14 OQ1 default — single source of truth `shell.zip` → install lifecycle → disk → `/app/*`; honours "shell is an extension like any other" with no kernel-baked byte symbols). **OQ2 pin: defer browser harness to 0.6.0.N**. **OQ3 pin: countdown + disabled submit**. **L.\* deferred to 0.6.0.N** (architect-signed-off this session) — folds into the same test-fixture buildout that absorbs the 25-case backfill and the headless-browser harness. `register_shell_routes` slots in at [main.cpp:478](src/kernel/main.cpp:478) AFTER `asset_server::restore_routes` so `/app/(.*)` does NOT shadow `/api/*` (B.06 structural enforcement; manual smoke verifies). Followed up by **0.6.0.1 atexit audit-shutdown ordering fix** (untagged, 2026-04-27) closing the broker-stop teardown SEGV (5/5 cores from same-day manual smoke); see CHANGELOG `2026-04-27 — 0.6.0.1 …` entry._
- [ ] 0.6.0.N Architecture session: contract-docs proposal (paper)   [strong]
   Decides adoption of the living-subsystem-contracts proposal in `docs/discussion/DISCUSSION-living-subsystem-contracts.md` (proposes a third tier `docs/contracts/` containing engine-agnostic, re-implement-bar-passing subsystem contracts that consolidate cross-milestone shape; milestone ICDs continue unchanged as authoring handoff + history; re-eval gains a roll-forward step). Resolves: adopt-or-not, naming convention, repo placement, re-eval-step wording in METHODOLOGY §Phase 3, `[contract-fill]` ROADMAP tag. Output if adopted: METHODOLOGY update + first contract-fill scheduled. Seven candidate subsystems are ready (auth+sessions+PATs, RBAC, capability registry, async bridge + QuickJS runtime, package install+lifecycle, audit, realtime pipeline post-0.5.5). Not critical-path for 0.6.0 code.
- [x] 0.6.0.N Architecture session: extension HTTP surface (paper)   [strong]
   _Shipped 2026-04-29 (untagged per `feedback_tagging_rule.md`). Branch `feat/0.6.0.N-arch-session-ext-http-surface`. Ratified the catch-all + manifest-prefix + runtime-route-table proposal in `docs/discussion/DISCUSSION-extension-http-surface.md` with nine commitments now normative at `architecture/05-extensions.md §6 Extension HTTP Surface`: (1) **principle** — kernel owns primitives, extensions own application surfaces (test: can the route be described without naming an application/protocol?); (2) **shape** — one catch-all kernel route + manifest-declared prefixes + install-time conflict check + runtime prefix → handler-reference table (`shared_mutex` mirroring `enforcement.cpp`) + kernel-side PAT auth pre-dispatch + execution-mode-agnostic handler reference; (3) **manifest schema** — three new fields `http_prefixes` / `unauthenticated_prefixes` / `handler_mode` (`quickjs` / `sidecar` / `bundled_native`); (4) **prefix-claim semantics** — full-prefix exclusive ownership, no method scoping, no host scoping in single-tenant (multi-tenant deferred); (5) **uninstall** — `draining` flag + drain in-flight + 503 on new requests + configurable timeout; (6) **privilege** — RBAC-gated install (`packages.install.with_http_prefixes` admins-only default; `unauthenticated_prefixes` separately gated more privileged); (7) **audit** — non-skippable `extension.http.dispatched` per request (PAT identity, prefix, extension, method/status, optional latency); (8) **performance contract** — ≤ 100 μs starting figure for catch-all overhead vs fixed-prefix kernel routes (final threshold pinned in ICD); (9) **implementation slot** — 0.6.7. Two `reserved (planned)` forward-reservations added to `architecture/05-extensions.md §2`: `/docs/*` (kernel markdown help reader) + `/api/docs/*` (kernel dynamic API discovery / OpenAPI / Swagger), both load-bearing today via install-time conflict check. ICD-authoring slot scheduled at `0.6.6.N`; implementation milestone at `0.6.7`. Discussion doc gains a `**Status (2026-04-29):** Ratified` header but body unchanged. `architecture/05-extensions.md §2.1` qualified (no longer "Rejected: Extension-Owned Arbitrary HTTP Routes"; renamed "Extension HTTP Surface — no arbitrary routes; manifest-declared, conflict-checked prefixes only" with a §6 forward-cite); §5.2 site-host extension status-noted as reframed by §6 + `unauthenticated_prefixes` for the public-page case; §5.3 "Rejected (Not Deferred)" first bullet narrowed to runtime-mutable arbitrary registration only. Plan file at the archived implementation record (architect-approved). Paper-only — no code, no tests, no migrations._
- [x] 0.6.0.N ICD-0.6.1 authoring (paper follow-up)   [strong]
   _Shipped 2026-04-29 (untagged per `feedback_tagging_rule.md`). Branch `feat/0.6.0.N-icd-0.6.1-authoring`. Authored `docs/icd/ICD-0.6.1-shell-schema-user-preferences.md` (2251 lines, 15 sections + 3 appendices). Three contributions land: (1) **bundled-shell first-boot install lifecycle** — pre-flight `ensure_bundled_shell_installed(cfg, db)` queries `plinth.packages` for an `ACTIVE` `provenance='bundled' name='shell'` row at boot and on absence reads `<bundle_path>/shell.zip` from disk, hands bytes to `install_lifecycle::install_package`; new `Provenance::Bundled` enum value + CHECK widening; five hard-fail audit codes; (2) **`frontend.mount` manifest-driven mount declaration** — `frontend.mount` + `frontend.entry` manifest fields become load-bearing, `parse_manifest` validates regex + reserved-prefix conflicts, `plinth.packages.frontend_mount/entry` columns + partial unique index enforcing active-frontend singleton, replaces ICD-0.6.0 §8 static-handler with `register_active_frontend_routes` reading bytes from `<data_dir>/extensions/<name>/<version>/client/`; (3) **`ext_shell` PG schema + `user_preferences` table + get/set capability pattern** — schema lands through ICD-0.4.3 path with `name='shell'` reserved, `(user_id, key, value JSONB, updated_at)` table with 64 KiB JSONB cap + `ON DELETE CASCADE`, three new capabilities (`shell.preferences.get` / `set` / `get_all`) gated by two new RBAC rules (`shell.preferences.read` / `shell.preferences.write`) with `users` group default-grants. 35 test cases enumerated (6 B.\* + 8 M.\* + 4 S.\* + 14 P.\* + 3 I.\*); 7 OQs with architect recommendations (two atomic capabilities + get_all; eager bulk hydrate; `set(undefined)` deletes; reserved-name reject at parse_manifest; literal `share/plinth/bundled` default; mount-conflict reject at install; 60 s audit dedup TTL). Discharges all four ICD-0.6.0 §15 `Closes: 0.6.1` deferral pointers (lines 1010–1040 of ICD-0.6.0): bundled-package first-boot install lifecycle, `frontend.mount` manifest contract, `ext_shell` PG schema, `user_preferences` table + get/set pattern. Discharges ICD-0.6.0 OQ1 architect override pinning bundle byte source = on-disk installed shell. Architecture-amendment forwards cited in CHANGELOG (architecture file edits land in the 0.6.1 code session per paper-session convention; see ICD-0.5.4 / ICD-0.5.5 precedent). Paper-only: no code, no tests, no migrations, no architecture file changes; verification is markdown cross-reference walk only._
- [x] 0.6.1 Shell schema + user preferences: `ext_shell` schema via standard extension migration, `ext_shell.user_preferences` table, get/set pattern as kernel capability calls   [medium]
   _Shipped 2026-04-29 (tag `v0.6.1`). Branch `feat/0.6.1-shell-schema-user-preferences`. Sixth code milestone of the 0.6.x Frontend Shell arc. Five-phase commit arc against ICD-0.6.1 (2251 lines, paper-authored 2026-04-29 prior session). Three contributions: (1) **bundled-shell first-boot install lifecycle** moves the byte source from kernel-baked linker-embedded blob to on-disk `<bundle_path>/shell.zip` per the 0.6.0 OQ1 architect override; new `src/kernel/shell/firstboot.{hpp,cpp}` + `Config::Shell::bundle_path` field; five hard-fail codes per ICD §3.5; three single-shot audits per §10.1; decommissions `install_shell_if_needed` + `kernel/packages/shell_blob.{hpp,cpp}` + the linker-embed CMake blocks; (2) **manifest-driven dispatch** via `src/kernel/shell/active_frontend.{hpp,cpp}` (replaces ICD-0.6.0's hardcoded `static_handler`); reads `frontend_mount` + `frontend_entry` from the active row; new `frontend_entry` column on `plinth.packages` with `chk_frontend_pair` CHECK; `parse_manifest` gains `is_bundled` parameter for the new `manifest.name.reserved` rule (parse-time per ICD §5.5 / OQ4); (3) **`ext_shell` PG schema + `user_preferences` table + get/set capability pattern** — `client/shell/migrations/001_init.sql` per ICD Appendix A; `client/shell/{capabilities,rbac}.json` populated with three capabilities + two RBAC rules + `default_grants[]`; three JS handlers under `client/shell/server/handlers/`; new general `default_grants` infrastructure in `rbac_manifest` + `install_lifecycle::register_extension_rbac_rules` (idempotent INSERT...SELECT). All seven OQs pinned per ICD §14 architect-recommendation defaults; ICD-0.6.1 §17 amendment block lands the resolutions plus six implementation deviations (group name `everyone` not `users`; rbac.json field `rule` not `name`; handler files dotted not underscored; `"any"` accepted as manifest param type; full P.\*/I.\* JS-dispatch suite deferred to 0.6.1.N; upgrade_package parses with USER provenance). 6 B.\* + 6 M.\* + 9 P.s.\*/P.r.\*/P.h.\* tests ship; full P.\*/I.\* JS-dispatch suite deferred to 0.6.1.N. Discharges all four ICD-0.6.0 §15 `Closes: 0.6.1` deferral pointers. Plan file at the archived implementation record (architect-approved)._
- [x] 0.6.1.N ICD-0.6.2 authoring (paper follow-up)   [medium]
   _Shipped 2026-04-29 (untagged per `feedback_tagging_rule.md`). Branch `feat/0.6.1.N-icd-0.6.2-authoring`. Authored `docs/icd/ICD-0.6.2-design-tokens-theme-scaling.md` (15 sections + 5 appendices). Four contributions: (1) **named CSS-custom-property token system** — 25 tokens lifted verbatim from `Plinth Shell.html:23-73`'s `:root` blocks (surface scale `--bg-0..4`, text scale `--text-0..3`, border scale, accent + `-soft` alphas, semantic tones, geometry radii, focus-ring); names frozen for the 0.6.x arc; baseline shifts to `:root { font-size: 13.5px }` so `1 rem == 13.5 px` at 100%; (2) **theme toggle** — persisted at `shell.theme` (JSONB string literal `light`/`dark`/`system`); `system` follows `matchMedia('(prefers-color-scheme: dark)')` with change listener; resolved value writes to `documentElement.dataset.theme`; token table swaps via `:root[data-theme="light"]` selector; (3) **UI scaling (80%–175%, rem-based)** — **architect-pinned rem-vs-zoom decision** per 2026-04-29 plan-mode interaction. Architect reported the canonical `zoom`-vs-Floating-UI failure (popup drift from anchor proportional to `(zoom - 1) × offset` as scale gradient grows) — root cause is `zoom`'s decoupling of DOM coordinate space from rendered geometry; `getBoundingClientRect()` and mouse-event coordinates report in viewport pixels which interact inconsistently with `zoom`. Rem leaves coordinate space untouched; only computed sizes change; popups stay anchored. Persisted at `shell.scale_pct` (JSONB integer 80–175); mechanism `documentElement.style.fontSize = "${pct * 0.135}px"`; bundle's `Plinth Shell.html:85` `style.zoom` usage amended to rem-based in v0.6.2 *code* PR per METHODOLOGY §Phase 2 Constraint #4 (ICD Appendix B); (4) **`/api/frontend/tokens.css` indirection** — kernel handler reuses v0.6.1 `active_frontend.cpp` resolver, returns `302 Found` with `Location: /ext/{name}/{version}/css/tokens.css` + `Cache-Control: no-cache` per architecture/06-frontend.md §4.1; 503 with JSON diagnostic on singleton violations; shell self-references the same `/api/frontend/tokens.css` URL for upgrade-pickup symmetry; fonts/icons/manifest.json deferred (OQ5 default `tokens.css`-only). 24 test cases enumerated (4 B.\* + 8 T.\* + 8 S.\* + 3 I.\* + 1 R.\* popup-anchor regression at 80/100/175%; 5 U.\* deferred to browser harness). 7 OQs with architect recommendations (literal-string theme storage; integer scale; inline-script pre-paint resolver; server-side scale-bound enforcement; `tokens.css`-only scope; avatar-popover placement; inherit `shell.preferences.set` audit family). Discharges ICD-0.6.0 §15 design-tokens deferral. Architecture-amendment forwards cited (architecture file edits + design-bundle zoom→rem amendment land in v0.6.2 code session). Plan file at the archived implementation record (architect-approved). Paper-only: no code, no tests, no migrations, no architecture file changes; verification is markdown cross-reference walk only._
- [x] 0.6.2 Design tokens, theme, UI scaling: `:root` custom properties, light/dark/system toggle, 80%–175% scaling (rem-vs-zoom decision pinned in `0.6.1.N ICD-0.6.2 authoring`), persisted to `user_preferences`. Consumes `/api/frontend/*` indirection (see `architecture/06-frontend.md §4`).   [medium]
   _Shipped 2026-04-29 (untagged per `feedback_tagging_rule.md` until merge to main; `v0.6.2` tag cuts on architect merge). Branch `feat/0.6.2-design-tokens-theme-scaling`. Seventh code milestone of the 0.6.x Frontend Shell arc. Four-phase commit arc against ICD-0.6.2 (paper-authored 2026-04-29 prior session). Four contributions: (1) **named CSS-custom-property token system** at `client/shell/client/css/tokens.css` (Appendix A verbatim — 25 tokens; light overrides at `:root[data-theme="light"]`); baseline shifts to `:root { font-size: 13.5px }`; index.html mechanically rewritten hex → `var()` + px → rem; manifest 0.6.1 → 0.6.2; (2) **theme toggle** `light` / `dark` / `system` persisted at `shell.theme` with pre-paint resolver at `client/shell/client/prepaint.js` (external sync script — implementation deviation from §SC4 path (a) inline+hash; same pre-paint property without build-time hash recompute); matchMedia listener flips only when stored value is absent or `"system"`; (3) **UI scaling 80%–175% rem-based** persisted at `shell.scale_pct` (mechanism `documentElement.style.fontSize = "${pct * 0.135}px"`); architect-pinned rem-vs-zoom decision validated end-to-end — popover-anchor stability gate (R.01) at 80% / 100% / 175% shows 0.30 rem delta invariant (no `zoom`-style drift). Server-side validator in `preferences.set.js` enforces the well-known-key contract (allow-list theme + integer-in-range scale); (4) **`/api/frontend/tokens.css` kernel indirection** at `src/kernel/frontend/api_frontend.{hpp,cpp}` — own LIMIT 2 libpq query (reports n=0 vs n>1 distinction the cached active_frontend resolver collapses); 302 with `Location: /ext/{name}/{version}/css/tokens.css` + `Cache-Control: no-cache`; 503 + JSON diagnostic body for the singleton-violation cases. Avatar popover gets Theme + Scale `<select>`s above the existing Sign Out button. Five implementation deviations recorded per METHODOLOGY §Phase 2 Constraint #4 in ICD-0.6.2 §17 amendment block: (1) `/api/frontend/tokens.css` registers WITHOUT auth filter (shell self-references it pre-auth from login page); (2) pre-paint resolver as external sync script not inline+CSP-hash (preserves strict `script-src 'self'` without per-build hash recompute); (3) kernel-side persistence via `cap.call` deferred to 0.6.1.N JS-dispatch follow-up (shell ships localStorage-only persistence for v0.6.2; SCHEMA validator in `preferences.set.js` ships ready); (4) B.02 verifies URL construction with synthetic name+version rather than following the 302 (asset_server byte-serving covered separately); (5) T.\* / S.\* / I.\* JS-dispatch tests deferred to 0.6.1.N (matches v0.6.1 posture; manual FE smoke is the ship-acceptance gate). 4 B.\* tests ship at `tests/kernel/frontend/api_frontend_test.cpp` via `test_seam::dispatch_tokens_css` (35 assertions; 10/10 stability standalone). All seven §14 OQs pinned per ICD architect-recommendation defaults. Discharges ICD-0.6.0 §15 *Design tokens, theme, UI scaling* deferral. The 2026-04-27 design-bundle's `Plinth Shell.html:85` `style.zoom` usage amended to rem-based in the same PR per Constraint #4 (Appendix B). Plan file at the archived implementation record (architect-approved)._
- [x] 0.6.2.N ICD-0.6.3 authoring (paper follow-up)   [medium]
   _Shipped 2026-04-29 (untagged per `feedback_tagging_rule.md`). Branch `feat/0.6.2.N-icd-0.6.3-authoring`. Authored `docs/icd/ICD-0.6.3-panel-sdk-client-sdk.md` (15 sections + 5 appendices). Three contributions: (1) **Panel SDK (`plinth.panel.*`)** — eleven methods codified from `DESIGN-shell-v06x.md §4.1`, split 5 live (`onActivate`, `onDeactivate`, `setDirty`, `registerShortcut`, `getContext`) + 6 stub (`navigate`, `openFloat`, `onNavigationIntent`, `requestFocus`, `setTrayState`, `setTrayBadge`) per architect-pin; stub failure mode pinned per OQ1 — sync `throw NotImplementedError`, async reject Promise (never silent no-op); `onNavigationIntent` is a "stub-receiver" registering callback without firing until 0.6.6; injected via shell-side `makePanelApi` factory per `DESIGN-shell-v06x.md §10` constraint #2; (2) **Client SDK (`plinth.call` / `plinth.subscribe` / `plinth.useData`) + new `POST /api/cap/{capability}` HTTP route + `/api/frontend/sdk.js` indirection** — new kernel HTTP cap-dispatch endpoint with session-cookie + CSRF + dispatch filter chain, JSON `{ "args": [...] }` body, typed CapabilityError rejection on 4xx/5xx mirroring `cap_bindings.hpp`; single shell-managed multiplex WS for subscribe per OQ4; `useData` Preact hook with React-Query stale-on-error semantics per OQ5; SDK ships at `client/shell/client/sdk.js`; second route on `api_frontend.{hpp,cpp}` (302 → versioned asset; mirrors v0.6.2 `tokens.css` pattern); import-map specifier `@plinth/frontend/sdk` per `architecture/06-frontend.md §4.3`; (3) **`frontend.boundary.caught` audit family promotion** from ICD-0.6.0 §10 deferral — top-level Preact boundary's `componentDidCatch` emits via single-purpose `audit.emit_boundary` capability (NOT direct `audit.log` exposure to browser per §10 SC2 — prevents action-name forgery); detail JSON `{ panel_id, error_message, error_stack?, component_path? }` length-capped (1024/8192/8192); `error_stack` omitted in production builds per OQ6; non-forgeable identity payload filled by kernel writer per `audit_bindings.cpp:44-56`; new RBAC rule `frontend.boundary.audit_emit` default-granted to `everyone` for admin-revoke posture. 39 test cases enumerated across 9 categories (B.\* / L.\* / C.\* / S.\* / U.\* / A.\* / K.\* / R.\* / I.\*); 6 ship library-level in v0.6.3, rest defer per existing JS-dispatch / browser-harness blockers carry-forward. 7 OQs with architect recommendations. Three architect-pinned decisions locked at the 2026-04-29 plan-mode interaction: (a) defer ICD-0.6.4 OQ4 (panels-query API) to ICD-0.6.4 paper per `feedback_icd_horizon.md`; (b) test extension is **test-only fixture** at `tests/extensions/sdk_demo/`, not bundled — production data_dir unchanged from v0.6.2 except shell.zip upgrade; (c) 5-live / 6-stub method split. Architecture-amendment forwards cited (`architecture/06-frontend.md §4.1` endpoint table, §4.3 import-map binding, §5 panel-system summary; `DESIGN-shell-v06x.md §0.6.3` status footer; all land in v0.6.3 code session per paper-session convention). No design-bundle amendments this milestone (bundle's JSX prototypes already match the SDK shape). Plan file at the archived implementation record (architect-approved). Paper-only: no code, no tests, no migrations, no architecture file changes; verification is markdown cross-reference walk only._
- [x] 0.6.3 Panel SDK + client SDK: `plinth.panel.*` lifecycle, `plinth.call()`, `plinth.subscribe()`, `plinth.useData()`, proven with test extension   [medium]
   _Shipped 2026-04-30 (untagged per `feedback_tagging_rule.md` until merge to main; `v0.6.3` tag cuts on architect merge). Branch `feat/0.6.3-panel-sdk-client-sdk`. Eighth code milestone of the 0.6.x Frontend Shell arc. Six-phase commit arc against ICD-0.6.3 (paper-authored 2026-04-29 prior session). Three contributions: (1) **Panel SDK** (`plinth.panel.*` 5-live + 6-stub at `client/shell/client/panels/panel_api.js` + loader at `client/shell/client/panels/loader.js` + import-map declared in index.html for `@plinth/frontend/sdk` + `preact` + `preact/hooks` + `htm`); (2) **Client SDK** (`plinth.call`/`plinth.subscribe`/`plinth.useData` at `client/shell/client/sdk.js` + new kernel `POST /api/cap/{capability}` route at `src/kernel/cap/api_cap.{hpp,cpp}` + `GET /api/frontend/sdk.js` 302 indirection extending the v0.6.2 `tokens.css` handler; vendored `preact-hooks.module.js` 10.22.0 same-origin); (3) **`ext.shell.frontend.boundary.caught` audit family** promoted from ICD-0.6.0 §10 deferral via single-purpose `shell.audit.emit` capability + RBAC rule default-granted to `everyone`. Test posture: 4 B.* HTTP-bridge cases ship; L.\*/C.\*/S.\*/U.\*/I.\*/A.\* (33 cases) defer to 0.6.1.N JS-dispatch follow-up (still blocked by `init_registry` teardown bug from test-fixture-buildout session 9). R.* enforced as exact-string asserts in `panel_api.js` NotImplementedError. Test-only fixture at `tests/extensions/sdk-demo/` per architect-pinned OQ4 (NOT bundled; production data_dir unchanged from v0.6.2 except `shell.zip` upgrade). Eight implementation deviations recorded in ICD-0.6.3 §17 amendment block (capability name renamed `audit.emit_boundary` → `shell.audit.emit` due to combined CF7 + rule-regex + namespace-match validator constraints; audit action `frontend.boundary.caught` → `ext.shell.frontend.boundary.caught` due to `audit.log()` `ext.` prefix requirement; CSRF deferred; RbacFilter omitted from cap-dispatch chain; `process.env.NODE_ENV` → `window.__PLINTH_PRODUCTION__`; URL→signature synthesis defaults version 1; effective_rules full-DB-expansion). All seven §14 OQs pinned per architect-recommendation defaults. Plan file at the archived implementation record (architect-approved)._
- [x] **0.6.3.N kernel-side dispatch + teardown hardening**   [strong]
   _Shipped 2026-04-30 (untagged per `feedback_tagging_rule.md`; rolls into v0.6.3 cut at architect's discretion). Branch `feat/0.6.3.N-dispatch-teardown-hardening`, single PR. Three bugs closed: (1) JS handler ctx injection — wrapper at `runtime_registry.cpp:298-307` now passes `(args, ctx)` two-positional; ctx carries audit-frame projection of UserContext (user{id,username,auth_type} + session_id + ip_address + call_depth + extension); also fixed two latent bundled-shell handler bugs surfaced by Bug 1 (`db.query` returns `{rows, row_count}` not the array; JSONB columns return as JSON-text and need `JSON.parse`). (2) `init_registry` test-fixture teardown — `ws_test_fixture::start_test_server` now mirrors production `main.cpp` init pair (`init_resolver` + `init_registry`) and atexit chain (`shutdown_registry` + `stop_notify_listener`) plus `static plinth::Config cfg` lifetime fix to keep `extensions::cfg_ptr` alive. (3) Drogon `EventLoopThreadPool::~` join-self — new shared header `tests/kernel/realtime/shared_pg_client.{hpp,cpp}` exposes process-lifetime PG client; 9 realtime TUs migrated; pool size 8 connections (mirrors production default). New tests: `P.04` (synthetic ctx-injection round-trip; `dispatch_extension_test.cpp`); `P.dispatch.01` (bundled-shell preferences round-trip via `extensions::dispatch`; `preferences_test.cpp`). Verification: `cmake --build --target tidy` clean (177 TUs); all 4 ctest groups green; 10× stability sweep on new tests. Three plan deviations recorded in CHANGELOG: B.shell.01 in api_cap_test deferred (the [ws] suite passing 81/81 already proves teardown is correct); two latent bundled-shell handler bugs fixed in commit 1 (otherwise Bug 1's "fix" wouldn't deliver any working capability); `delta_sync_test` migration reverted (advisory-lock interference in [ws] group). Plan file at the archived implementation record (architect-approved). **Unblocks 0.6.4 topbar work + the `0.6.3.N JS-dispatch test suite backfill` 33-case follow-up.**_

- [x] **0.6.3.N server/kernel JS-dispatch test-suite backfill**   [medium]
   _Completed 2026-09-03. Added 32 real bundled-shell dispatch cases: 14
   ICD-0.6.1 preference cases, 8 ICD-0.6.2 theme cases, 8 ICD-0.6.2 scale
   cases, and 2 ICD-0.6.3 boundary-audit cases. The suite exercises the
   resolver, RBAC, QuickJS handlers, PostgreSQL persistence, HTTP error
   mapping, audit attribution/deduplication, and registry teardown. Contract
   repairs distinguish absent values from JSON null, use `db.exec` affected
   rows for deletion, map invalid/oversized inputs to HTTP 400/413, and
   snapshot authenticated identity onto detached audit writes. Client SDK,
   WebSocket, Preact hook, panel lifecycle/switching, and full-stack browser
   cases remain explicitly deferred in `docs/DEFERRED.md`; the former
   "33-case" wording mixed those browser-only families with this executable
   server slice and was not a valid count._

- [ ] RE-EVAL following 0.6.3   [rewrite session]
- [ ] 0.6.3.N ICD-0.6.4 authoring (paper follow-up)   [medium]
   Authors `docs/icd/ICD-0.6.4-tabs-subtabs-home-launcher.md`. Scope: topbar reads `plinth.panels` (RBAC-gated) + app-switcher dropdown + sub-tab rendering + home-view icon grid + realtime install/uninstall. **Pins `data-ipoint` integration-point naming convention** per the 2026-04-27 design-bundle commitment — every shell-owned and extension-owned render seam carries a `data-ipoint` attribute naming the contract (cite `docs/sketches/shell-design-2026-04-27/project/shell/topbar.jsx`, `app.jsx`, etc.). Discharges ICD-0.6.0 §15 tabs/launcher deferral.
- [ ] 0.6.3.N ICD-0.6.x source-seq tracking authoring (paper follow-up)   [fuzzy]
   Authors a follow-up ICD that closes ICD-0.5.5 W.06 `superseded_seqs[]` design defer. Three options on the table: (a) plumb source-seq through coalescer→writer boundary, (b) re-introduce a peer-listener path for the side-channel, (c) discard the field entirely. Likely absorbed into ICD-0.6.3 if the SDK work in 0.6.3 binds the optimistic-update path tighter than expected. See `DEFERRED.md` 2026-04-26 entry W.06 bucket.
- [ ] 0.6.4 Extension tabs, sub-tabs, home launcher: topbar reads `plinth.panels` (RBAC-gated), app-switcher dropdown, sub-tab rendering, home-view icon grid, realtime install/uninstall. `data-ipoint` overlay pinned in `0.6.3.N ICD-0.6.4 authoring`.   [medium]
- [ ] 0.6.4.N ICD-0.6.5 authoring (paper follow-up)   [fuzzy]
   Authors `docs/icd/ICD-0.6.5-float-system.md`. Scope: float chrome + lifecycle + responsive transforms + state persistence + max-float limit. Discharges ICD-0.6.0 §15 float-system deferral.
- [ ] 0.6.5 Float system: float chrome (title bar, min/max/close, jump-to-app), lifecycle, responsive transforms, state persistence, max-float limit with oldest-minimized   [medium]
- [ ] 0.6.5.N ICD-0.6.6 authoring (paper follow-up)   [fuzzy]
   Authors `docs/icd/ICD-0.6.6-tray-content-type-navigation.md`. Scope: tray panel type + shell-owned bell/avatar as dogfooded trays + `chrome_essential` fallback + three-tier content-type handler resolution with `ext_shell.default_apps` + `navigate()` / `openFloat()` / `onNavigationIntent()`. Discharges ICD-0.6.0 §15 tray + content-type + navigation deferrals.
- [ ] 0.6.6 Tray system + content-type resolution + navigation intents: tray panel type, shell-owned bell/avatar as dogfooded trays, `chrome_essential` fallback, three-tier content-type handler resolution with `ext_shell.default_apps`, `navigate()` / `openFloat()` / `onNavigationIntent()`   [medium]
- [ ] 0.6.6.N ICD-0.6.7 authoring (paper follow-up)   [strong]
   Authors `docs/icd/ICD-0.6.7-extension-http-surface.md`. Scope: catch-all kernel route registration + `http_prefixes` / `unauthenticated_prefixes` / `handler_mode` manifest fields + install-time conflict checking against `architecture/05-extensions.md §2` (incl. `reserved (planned)` rows) + runtime route table thread-safety (`shared_mutex` per `enforcement.cpp` precedent) + execution-mode dispatch (`quickjs` / `sidecar` / `bundled_native`) + uninstall drain semantics + RBAC rules (`packages.install.with_http_prefixes` + `packages.install.with_unauthenticated_prefixes`) + `extension.http.dispatched` audit entry shape + final ≤ 100 μs catch-all overhead threshold + drain-timeout default and bounds. Discharges `architecture/05-extensions.md §6.2–§6.9` to a code-ready spec. See `architecture/05-extensions.md §6` for the ratified contract.
- [ ] 0.6.7 Extension HTTP surface — catch-all primitive + manifest prefixes + runtime route table   [strong]
   Implementation milestone for `architecture/05-extensions.md §6` (ratified 2026-04-29 paper session). Lands the catch-all kernel route + manifest-declared prefix conflict checking + runtime prefix → handler-reference table behind `shared_mutex` + kernel-side PAT auth pre-dispatch + execution-mode-agnostic handler resolution (quickjs / sidecar / bundled_native) + uninstall drain + 503 + RBAC-gated install + per-dispatch audit + ≤ 100 μs catch-all overhead acceptance gate. Precondition for any extension that wants HTTP surface — Files-Nextcloud-compat, CalDAV / CardDAV / S3-compat / ActivityPub federation, OAuth-flow surfaces all inherit this primitive without further architecture work.
- [ ] RE-EVAL following 0.6.7   [rewrite session]

## 0.6a — Admin Extension (parallel; gated on shell SDK milestones)
Admin extension (`DESIGN-admin-v06x.md`) is a second bundled package, installed alongside the shell on first boot. Version numbers TBD by architect; each milestone gates on the shell SDK feature it consumes (0.6.3 minimum).
- [ ] 0.6a-A Package management panel (install, list, upgrade, disable, uninstall) — gated on 0.4.x + 0.6.3   [fuzzy]
- [ ] 0.6a-B Groups management panel — gated on 0.1.4 + 0.6a-A   [fuzzy]
- [ ] 0.6a-C RBAC administration (rule browser, grant matrix, effective-rule inspector, policy explanation) — gated on 0.1.5 + 0.2.x   [fuzzy]
- [ ] 0.6a-D Sidecar management panel — gated on 0.8.x   [fuzzy]
- [ ] 0.6a-E Audit log browser — gated on 0.1.7   [fuzzy]
- [ ] 0.6a-F Schedules admin panel: package jobs + backups + retention sweeps + privacy-safe user-policy aggregate (admins see ceilings + cap grants + per-user counts + last-run timestamps; admins do NOT see reminder bodies, handler code, or per-user item lists). **Pins privacy contract** per the 2026-04-27 design-bundle commitment (cite `docs/sketches/shell-design-2026-04-27/project/shell/admin-schedules.jsx`); architecture amendment to `architecture/01-identity.md §2` lands in the same PR per METHODOLOGY §Phase 2 Constraint #4. Gated on 0.7.2 + 0.6a-A.   [fuzzy]

## 0.6b — Additional Bundled Extensions (parallel; gated on per-extension prerequisites)
Plinth ships **Shell + Admin** by default. Additional bundled extensions land here when they (a) demonstrate platform capabilities the architecture would otherwise need to mock, (b) are lightweight enough not to bloat plinth, and (c) replace kernel maintenance overhead of doing it raw. **Example apps** — Notes, Files, Homecare, Knowledge Base, user-scripting Automations — belong in a separate downstream consumer project, NOT plinth scope. The 2026-04-27 design bundle's `notes.jsx` / `homecare.jsx` / `kb.jsx` / `automations.jsx` are visual reference for that consumer project's eventual shape, not plinth-bundled extensions.
- [ ] 0.6b-A Perf system tray bundled extension: live tray icon (kernel/PG/extension health, animated SVG status indicator) + click-to-open dashboard modal (recent alerts, metric drilldown, RSS / CPU / event-rate cards → tabbed detail). Demonstrates the tray-as-live-surface contract from `DESIGN-shell-v06x.md §3.4` AND surfaces 0.7.1's `plinth.metrics` table without requiring a third-party metrics extension. Subsumes the previous 0.7.5 "Metrics dashboard" slot with a tray-first delivery shape. Gated on 0.7.1 + 0.6.6 (tray system).   [fuzzy]

The Logs/audit browser surface lives within the admin extension as 0.6a-E (panel within admin), not as a separate bundled extension here.

--- SCHEMA FREEZE — 0.7 onward: numbered immutable migrations only ---

## 0.7 — Metrics + Scheduled Tasks
- [ ] 0.7.0 Schema freeze: schema.sql → migrations/001_baseline.sql   [fuzzy]
- [ ] 0.7.1 plinth.metrics partitioned table (migration 002)   [fuzzy]
- [ ] 0.7.2 Scheduler: cron expressions, PG advisory lock grab   [fuzzy]
- [ ] 0.7.3 Default tasks: heartbeat sweep, session cleanup, metrics cleanup   [fuzzy]
- [ ] RE-EVAL following 0.7.3   [rewrite session]
- [ ] 0.7.4 Extension metrics registration via metrics.* API   [fuzzy]

_(0.7.5 metrics dashboard subsumed into 0.6b-A Perf system tray bundled extension — see 0.6b for the tray-first delivery shape.)_

## 0.8 — Sidecar Tier
- [ ] 0.8.0 Sidecar registration endpoint, bootstrap token validation   [fuzzy]
- [ ] 0.8.1 Sidecar capability registration into registry   [fuzzy]
- [ ] RE-EVAL following 0.8.1   [rewrite session]
- [ ] 0.8.2 Tier 3 resolution: remote proxy dispatch   [fuzzy]
- [ ] 0.8.3 Circuit breaker, timeout, failure handling   [fuzzy]
- [ ] 0.8.4 Sidecar health endpoint polling, metrics collection   [fuzzy]
- [ ] 0.8.5 Admin: sidecar management UI   [fuzzy]
- [ ] RE-EVAL following 0.8.5   [rewrite session]

## 0.9 — HA
- [ ] 0.9.0 plinth.node_registry (UNLOGGED), heartbeat write   [fuzzy]
- [ ] 0.9.1 Stale node detection + self-eviction   [fuzzy]
- [ ] 0.9.2 Cross-node capability proxy (Tier 3 for extensions)   [fuzzy]
- [ ] 0.9.3 Cross-node sidecar routing   [fuzzy]
- [ ] RE-EVAL following 0.9.3   [rewrite session]
- [ ] 0.9.4 Multi-node metrics aggregation   [fuzzy]
- [ ] 0.9.5 WebSocket reconnect to different node, delta sync   [fuzzy]

## 0.10 — Storage + Notifications + Polish
- [ ] 0.10.0 File storage: filesystem backend, extension-scoped prefixes. Includes HTTP surface per `architecture/03-data.md §2.3` (`POST/GET/DELETE/GET-list` at `/api/storage/{extension}/{path...}`, resumable uploads for > 100 MB, Range on downloads, two-tier quota).   [fuzzy]
- [ ] 0.10.1 storage.* API in QuickJS runtime (capability-layer wrapper over the HTTP surface from 0.10.0)   [fuzzy]
- [ ] RE-EVAL following 0.10.1   [rewrite session]
- [ ] 0.10.2 Notification bus: in-app push via WebSocket   [fuzzy]
- [ ] 0.10.3 http.* API: outbound HTTP with admin allowlist   [fuzzy]
- [ ] 0.10.4 Extension hot-reload   [fuzzy]
- [ ] 0.10.5 plinth validate CLI hardening   [fuzzy]
- [ ] RE-EVAL following 0.10.5   [rewrite session]
- [ ] 0.10.6 Security audit pass   [fuzzy]
- [ ] 0.10.7 Documentation: EXTENSION-GUIDE.md finalized   [fuzzy]

## Testing & Security (cross-cutting, scheduled alongside milestones)
- [ ] Property-based tests on RBAC permission logic (0.1.5)
- [ ] Fuzz harnesses for every JS→C++ bridge function (0.3.0)
- [ ] ThreadSanitizer CI job for WebSocket + LISTEN/NOTIFY concurrency (0.5.x)
- [ ] DAST scan (ZAP or equivalent) against HTTP surface (0.6.x)
- [ ] Security audit pass (0.10.6, already tracked above)

## 1.0 — Stable Release
- [ ] 1.0.0 All open questions resolved, docs complete, API frozen   [fuzzy]
