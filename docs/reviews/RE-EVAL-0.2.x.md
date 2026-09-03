# Re-evaluation — following 0.2.x

**Date:** 2026-04-17
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3)
**Trigger:** `RE-EVAL following 0.2.x` item on ROADMAP, reached after v0.2.5 merged. 0.2.6 is trigger-gated (expected alongside 0.3.3) and does not block this re-eval.
**Cadence position:** First scheduled re-eval since the cadence was installed in the 2026-04-17 methodology docs session. `RE-EVAL following 0.2.x` is item 1 of 14 planned re-evals.
**Scope:** 0.1.x + 0.2.x. Both halves — code-aware first, then structural.

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index — §5 source tree, §8 open questions, §10 trust boundary)
- `docs/architecture/01-identity.md` (all sections; §3 anonymous identity; §4 user-deletion contract — new in 0.1.8)
- `docs/architecture/02-capabilities.md` (all sections; §1.3 three-tier resolution; §1.12 composition reservation; §2 kernel standard library; §3 QuickJS async bridge pointer)
- `docs/architecture/04-services-ha.md` §1 audit logging

### ICDs (0.1.x + 0.2.x)
- `ICD-0.1.2-auth-sessions.md`, `ICD-0.1.3-pats.md`, `ICD-0.1.4-groups-rbac.md`, `ICD-0.1.5-rbac-enforcement.md`, `ICD-0.1.6-websocket.md` (incl. its *Implementation Notes (0.1.6)* footer), `ICD-0.1.7-audit.md`
- `ICD-0.2.0-capability-registry.md`, `ICD-0.2.2-capability-resolution.md`, `ICD-0.2.4-capability-rbac.md`
- No ICDs exist for 0.2.1 (parser), 0.2.3 (LISTEN/NOTIFY), 0.2.5 (`cap.batch()`). Their contracts were implicit in ICD-0.2.2 and the CHANGELOG.

### Code
- `src/kernel/` full file enumeration (54 files)
- Spot-reads: `main.cpp`, `capabilities/{resolution,batch,types}.hpp`, `audit/handlers.{hpp,cpp}`, `auth/handlers.cpp`, `ws/subscriptions.hpp`
- `tests/kernel/` directory structure; grepped for anonymous-identity test coverage

### Discussion / design context
- Four `docs/discussion/DISCUSSION-*.md` files (all authored during 0.1.8 but **never committed** — see §2.4 below; landed in this PR)
- `docs/design/` — 8 DESIGN-*.md files; confirmed authority references are intact

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. Each has a proposed resolution; disposition below (§4) records what was fixed in this session vs. what became a new roadmap item.

### 2.1 Interface-drift — ICD-0.2.2 dispatch signature

**ICD-0.2.2 §Dispatch Contract** declares:

```cpp
Task<Result<CapabilityResult>> callCapability(const CapabilityCall&, const UserContext&);
```

**Code** (`src/kernel/capabilities/resolution.hpp`) ships synchronous:

```cpp
auto call_capability(const CapabilityCall&, const UserContext&) -> ResolveResult;
```

The deviation is well-documented in the header's "Deviations from ICD-0.2.2" block (reasons: no coroutine users in-repo yet; async wrapper deferred to 0.2.6, triggered by first real coroutine caller alongside 0.3.3). CHANGELOG entries v0.2.2, v0.2.4, v0.2.5 all re-acknowledge the deviation. But **the ICD text itself was never amended**. A future session consulting only the ICD misses the deviation.

**Resolution:** Amend ICD-0.2.2 §Dispatch Contract with an "Implementation deviation (0.2.2 → 0.2.6)" subsection — patterned on the ICD-0.1.6 *Implementation Notes* footer precedent. The 0.2.6 roadmap item remains the commitment to the ICD's `Task<>` target. **Fixed in this session.**

### 2.2 Interface-drift — ICD-0.2.2 `CapabilityCall` struct shape

ICD-0.2.2 declares `CapabilityCall` with separate `namespace_`, `version`, `function` fields plus `args`, `user_id`, `call_depth`. Code uses a pre-composed `signature` string (parsed inside the resolver), no per-call `user_id` (scope precedence is driven by `UserContext::user_id`). Both are internally consistent and the end-to-end behaviour matches the §Resolution Algorithm, but the declared struct shape in the ICD is not what callers will actually instantiate in 0.3.x when the JS bridge writes to this API.

**Resolution:** Amend ICD-0.2.2 §Dispatch Contract to reflect the `signature`-string form with a short rationale ("the bridge already holds the full string; the resolver parses once at step 1"). **Fixed in this session.**

### 2.3 Interface-drift — ICD-0.2.2 `UserContext` fields

ICD-0.2.2 `UserContext` has 4 fields: `{user_id, username, auth_type, effective_rules}`. Code's `UserContext` has 6 — adds `session_id` and `ip_address` for `capability.rbac.denied` audit enrichment, added in 0.2.4 but never written back into ICD-0.2.2.

**Resolution:** Amend ICD-0.2.2 `UserContext` to include the two audit fields with a marker "(added in 0.2.4)". **Fixed in this session.**

### 2.4 Interface-drift — ICD-0.2.4 rule-lookup database fallback

ICD-0.2.4 §Rule Lookup Ordering lists three steps: Tier 1 map, Tier 2 cache, **Database fallback**. Code (`resolution.cpp`) deliberately omits step 3 — cache miss resolves to `capability_not_found` to keep the hot path DB-free. Deviation documented in `resolution.hpp` deviation block and CHANGELOG v0.2.4 entry. ICD never amended.

**Resolution:** Amend ICD-0.2.4 §Rule Lookup Ordering to replace step 3 with "cache miss is a negative result; the hot path is DB-free", with a rationale paragraph mirroring the header comment. **Fixed in this session.**

### 2.5 Arch-silent-on-code — `reload_tier2_cache` + LISTEN-reconnect resync

The `reload_tier2_cache(db_cfg)` helper and the policy *"listener calls it after every successful LISTEN open so missed-NOTIFY divergence is bounded by one reconnect backoff"* landed as a post-ICD amendment during 0.2.4 (the 2026-04-17 architect question about missed-NOTIFY TTL). Neither ICD-0.2.2 nor ICD-0.2.4 nor `architecture/02-capabilities.md` describes it. The architecture merely says "stale cache window (tens of milliseconds)" is acceptable, which is true for steady-state NOTIFY but silently ignores the reconnect gap.

**Resolution:**
- Append a "LISTEN-reconnect resync" paragraph to `architecture/02-capabilities.md §1.3` (Tier 2 Local-node) under the "Stale cache window" bullet.
- Add `reload_tier2_cache()` to ICD-0.2.2 public API list, referenced from §Cache Invalidation.
**Both fixed in this session.**

### 2.6 Arch-silent-on-code — `BatchResult::failed_index`

Code's `BatchResult` has a third field, `failed_index`, pointing at the element that aborted the batch — useful for audit/debug. ICD-0.2.2 §cap.batch() Behavioral Contract says nothing about it (documented as deviation #c in CHANGELOG v0.2.5).

**Resolution:** Amend ICD-0.2.2 §cap.batch() Behavioral Contract to include `failed_index` as part of the returned shape, and add the "sequential is a conforming initial implementation" clarifier that CHANGELOG v0.2.5 already records. **Fixed in this session.**

### 2.7 Code-diverged-from-arch — `ARCHITECTURE.md §5` source tree layout

Arch §5 describes a kernel tree that has diverged substantially from what shipped through 0.2.5:

| §5 says | Reality |
|---|---|
| `rbac/ {enforcer, validator}` | `rbac/ {enforcement}` — single module, both concerns co-located |
| `capabilities/ {registry, parser, resolver, cache}` | `capabilities/ {batch, bootstrap, listener, parser, registration, resolution, types, validation}` — same scope, different decomposition |
| `dispatch/ {local, proxy, sidecar, batch}` | not present; batch.cpp lives in `capabilities/` |
| `db/ {postgres, schema, migration}` | `db/ {bootstrap}` only — schema/migration logic inside `bootstrap.cpp` |
| `realtime/ {broker, coalescer, delta}` | `ws/ {auth_flow, close_codes, conn_state, connection_registry, events_controller, heartbeat, messages, publish, registration, subscriptions}` — different name, different decomposition, built for 0.1.6 WebSocket scope |
| (not listed) `auth/ {crypto, handlers, middleware, pat_handlers, rate_limiter}` | shipped 0.1.2–0.1.3 |
| (not listed) `groups/ {handlers}` | shipped 0.1.4 |
| (not listed) `logging.{hpp,cpp}` at kernel root | shipped 0.1.6 |

§5 is partially *arch-ahead-of-code* (naming future directories that don't exist yet — `scripting/`, `scheduler/`, `notifications/`, `metrics/`, `packages/`, `ha/`, `api/`, `storage/`) and partially *arch-stale-vs-code* (the `dispatch/` and `realtime/` names don't match what shipped).

**Resolution:** Rewrite ARCHITECTURE.md §5 into two passes — (a) **current** layout through 0.2.5, and (b) **forward-looking** directories expected by later milestones. Flag `ws/` ↔ `realtime/` as an open question to settle during 0.5.0 (keep `ws/`, or rename to `realtime/` when the coalescer lands). **Fixed in this session** (§5 rewrite + open question); rename decision itself remains open.

### 2.8 Missing-test-for-arch-claim — anonymous identity safeguard

`architecture/01-identity.md §3` is explicit:

> **Enforcement test (required in 0.1.5).** A test case asserting that `UserContext::anonymous()` is rejected by every RBAC-gated route until a rule is explicitly granted to `everyone`. This test is the permanent safeguard against accidental public exposure of authenticated endpoints.

Verification:
- `grep -ri anonymous src/` → no matches.
- `grep -ri anonymous tests/` → no matches.
- No `UserContext::anonymous()` factory exists in code; no `everyone`-only test case in `tests/kernel/rbac/enforcement_test.cpp`.

0.1.5 shipped without this test. The §3 language is strong ("required", "permanent safeguard") — this is a real gap, not a nice-to-have.

**Resolution:** New roadmap item (strong-band, catch-up) to land the anonymous-identity test and the `UserContext::anonymous()` factory it asserts against. **Added to ROADMAP in §5 below.**

### 2.9 Missing-validation — ICD-0.2.2 §Performance Targets

ICD-0.2.2 §Performance Targets sets < 1μs Tier 1 and < 1ms Tier 2 targets and states *"These targets should be validated via benchmarks during 0.2.5 or a dedicated performance pass."* 0.2.5 shipped `cap.batch()` without benchmarks; no benchmark infrastructure exists in the repo (no `benchmarks/` directory, no Google Benchmark dependency).

**Resolution:** New roadmap item — a small performance-pass milestone to land benchmark infrastructure and validate the Tier 1/2 targets. Alternative: relax the ICD language from "should be validated" to "are targets; validation is deferred". Prefer the former — the targets are quoted by `architecture/04-services-ha.md §3.1` as *"Capability resolution latency per tier (1/2/3)"*, and real numbers here feed the metrics story. **Added to ROADMAP in §5 below.**

### 2.10 Arch-silent-on-repo — `docs/discussion/` directory was never committed

Four discussion docs authored during 0.1.8 (`DISCUSSION-cross-cutting-composition.md`, `DISCUSSION-ai-bridge.md`, `DISCUSSION-streaming-and-media.md`, `DISCUSSION-ha-scale-and-offload.md`) were cited from `architecture/02-capabilities.md §1.12` and from `architecture/05-extensions.md §4` but never committed to the repo. They were delivered to this session via `files.zip` at the repo root.

**Resolution:** Land `docs/discussion/*.md` in this PR (step 0 of the session). **Fixed in this session.**

### 2.11 Observation — recurring deferral pattern (see §6 for methodology suggestion)

Not a gap on its own; recorded for the methodology observations in §6.

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.1.2 — Session + PAT tables, argon2id params, token format.** Schema matches; `plinth.users`, `plinth.sessions`, `plinth.pats` column names and constraints match ICD.
- **ICD-0.1.2 §First-user bootstrap.** Code correctly adds first user to `admin` group via the bootstrap-compatible path (`src/kernel/auth/handlers.cpp` lines ~77–154).
- **ICD-0.1.3 §Authentication middleware branching.** `plinth_` prefix check and PAT-vs-session fork work as declared; `last_used_at` fire-and-forget update is non-blocking.
- **ICD-0.1.4 §Groups / members / rules tables.** Schema matches. Built-in-group protection (`built_in = true`) enforced. `bootstrap_groups` registers `admin` and `everyone` and grants `kernel.admin` to `admin` per architecture §2.3.
- **ICD-0.1.5 §RBAC filter.** `RbacFilter` implements the additive-union algorithm; fail-closed on errors; all denials audit via `log::audit`. (Exception: §2.8 anonymous test is missing — see above.)
- **ICD-0.1.6 §Connection lifecycle + heartbeat + single-WS-per-key.** Implementation Notes footer honestly captures the five scope reductions taken during 0.1.6 — no hidden deviations.
- **ICD-0.1.7 §/api/audit endpoint + kernel.admin gate.** Endpoint wired via `plinth::rbac::register_rule_requirement(Get, "/api/audit", {"kernel.admin"})` — matches ICD §Endpoints.
- **ICD-0.2.0 §Registration + disable/enable/deregister lifecycle.** All four functions land; `capability.registered`, `capability.deregistered`, `capability.extension_disabled`, `capability.extension_enabled` all emit. Sync-libpq deviation is documented; no hidden deviations. `NOTIFY plinth_capability_changed` fires on every mutation.
- **ICD-0.2.2 §Three-tier resolution algorithm.** Parse → depth check → RBAC → Tier 1 → Tier 2 → Tier 3 stub, in order. Tier 3 returns `tier3_not_available`. Call-depth enforcement works at the documented limit.
- **ICD-0.2.4 §Per-hop RBAC + kernel.admin universal match + audit on deny.** All three verified against `resolution.cpp` and `tests/kernel/capabilities/resolution_test.cpp`. Nine Catch2 cases cover every ICD exit-criterion bullet per CHANGELOG v0.2.4.
- **`architecture/02-capabilities.md §1.1` Contract format** (namespace:version:function, exact-match, version is integer). Code parser (`parser.cpp`) rejects non-integer version, non-matching namespace pattern, etc.; 18-seed fuzz corpus in `tests/kernel/capabilities/fuzz_parser_seeds/` covers edge cases.
- **`architecture/01-identity.md §4` user-deletion contract.** This is a reservation, not an obligation — the ICD milestone is 0.10.x. No code exists yet, and none should; no drift.
- **`DESIGN-logging-subsystem.md` async spdlog path.** 0.1.7 migrated to `async_logger`; `g_audit_ready` atomic gates the Drogon dependency so tests don't crash at exit.
- **Audit event catalog** (ICD-0.1.7 §Audit Event Catalog). Every action listed has at least one code site emitting it; `capability.rbac.denied` added in 0.2.4 with `call_depth` field as specified.
- **Fuzz / sanitizer coverage.** libFuzzer parser harness (0.2.1 / 0.2.1.1) plus ASAN/UBSan CI (0.1.4) plus TSan smoke in `batch_test.cpp`. All present, CI-wired.

---

## 4. Disposition — what was fixed here vs. scheduled

### Fixed in this PR (doc-only)

1. ICD-0.2.2 § Dispatch Contract — deviation note, sync-form signature, updated `CapabilityCall` shape, updated `UserContext`, `reload_tier2_cache` listed.
2. ICD-0.2.2 § cap.batch() — `failed_index`, sequential-initial-impl clarifier.
3. ICD-0.2.4 § Rule Lookup Ordering — DB-fallback removed; cache-miss-is-not-found documented.
4. `architecture/02-capabilities.md §1.3` — LISTEN-reconnect resync paragraph.
5. `ARCHITECTURE.md §5` — rewritten into current + forward-looking passes; `ws/` vs `realtime/` flagged as open question settled at 0.5.0.
6. `docs/discussion/*.md` — four files landed (previously zip-only).
7. ROADMAP — band labels unchanged; three new items added (§5 below); `ws/` vs `realtime/` naming decision added to ARCHITECTURE §8 open-question list.
8. CHANGELOG — this session's entry.

### Scheduled as new roadmap items (see §5)

- Anonymous-identity safeguard test (0.1.5 catch-up, strong-band).
- Capability Tier 1/2 benchmark validation (deriving from ICD-0.2.2 §Performance Targets, strong-band).
- Architecture session to write ICDs for 0.3.0 / 0.3.1 / 0.3.2 (unlocks band promotions + 0.3 code work, strong-band).

---

## 5. Paper pass

### 5.1 Band label review

Per the Plinth labeling rule declared in CHANGELOG 2026-04-17 (*"strong = ICD content exists; medium = DESIGN doc exists, ICDs not yet written; fuzzy = sketch only"*), the current labels are consistent with artifact reality:

| Milestone | Current | Review outcome |
|---|---|---|
| 0.2.6 | strong | No change. ICD-0.2.2 specifies the `Task<>` target; 0.2.6 is trigger-gated but content-specified. |
| 0.3.0 – 0.3.2 | medium | No change. DESIGN-quickjs-bridge.md covers them; ICDs not yet written. Promotion gated on the new ICD-writing roadmap item (§5.2 below). |
| 0.3.3 – 0.3.5 | medium | No change. Same rationale. |
| 0.4.x | medium | No change. DESIGN-packages-v04x.md covers them. |
| 0.5.x | medium | No change. `architecture/03-data.md §3` provides the design authority in lieu of a separate DESIGN doc. |
| 0.6.x | medium | No change. DESIGN-shell-v06x.md covers them. |
| 0.6a-* | fuzzy | No change. DESIGN-admin-v06x.md is a stub; real content arrives alongside shell SDK. |
| 0.7.x – 0.10.x, 1.0.0 | fuzzy | No change. |

**No band promotions or demotions this session.** This is acceptable per METHODOLOGY §3.3 *Structural-only re-evaluations* — Plinth just completed its first-ever band-labeling pass five days before this re-eval, and no ICDs have been written since 0.2.5. A zero-band-change outcome here is legitimate; the code-aware half is the substantive output.

### 5.2 New roadmap items

Three additions, inserted in sequence position before the milestones they unlock:

Numbered as post-milestone companions under 0.2.6 per Plinth precedent (0.2.1 → 0.2.1.1, 0.1.5 → 0.1.5.1):

**0.2.6.1 — Anonymous-identity enforcement test** `[strong]`
Catch-up item. Lands `UserContext::anonymous()` plus a test case asserting every RBAC-gated route rejects anonymous until `everyone` is explicitly granted a rule. Required by `architecture/01-identity.md §3`; absent since 0.1.5.

**0.2.6.2 — Capability Tier 1/2 benchmark validation** `[strong]`
Lands a `benchmarks/` directory, Google Benchmark dependency, and two harnesses validating ICD-0.2.2 §Performance Targets (< 1μs Tier 1, < 1ms Tier 2). Small footprint; unblocks the metrics story (`architecture/04-services-ha.md §3.1`).

**0.2.6.3 — Architecture session: ICDs for 0.3.0 / 0.3.1 / 0.3.2** `[strong]`
Scale-3 arc session (QuickJS integration is the hardest engineering problem per `architecture/02-capabilities.md §3`). Produces three tightened ICDs; unblocks code work and enables a future re-eval to promote 0.3.0–0.3.2 from `[medium]` to `[strong]`. Precondition for any 0.3.x code work per METHODOLOGY §Phase 1.

### 5.3 Discussion docs — promote / archive assessment

Four files examined. Current timestamps put them at 1–2 code milestones of shelf time (authored during 0.1.8, now at 0.2.5). METHODOLOGY §3.1 flags docs sitting in `discussion/` "more than a few milestones without movement" for promotion or archive.

| File | Feeds | Assessment |
|---|---|---|
| `DISCUSSION-cross-cutting-composition.md` | Future `DESIGN-composition-v09x.md` (~0.9.x) | Parked. Far horizon. Kernel-side constraints already reflected in `architecture/02-capabilities.md §1.12` + `architecture/05-extensions.md §4`. No action. |
| `DISCUSSION-ai-bridge.md` | Extension-layer, ~0.12–0.13 | Parked. Extension work, off kernel roadmap. References are all "don't-foreclose" checks against the kernel; all currently satisfied per §7 of the doc itself. No action. |
| `DISCUSSION-streaming-and-media.md` | Client SDK spec (0.6.3), sidecar contract (0.8.x), storage HTTP (0.10.x), and notably *"keep `plinth.call()`'s return value opaque at the bridge (0.3.x)"* | Parked but **flag for review by the 0.2.6.3 arch session** (the ICD-writing session for 0.3.0–0.3.2). The 0.3.x bridge should not foreclose the streaming return-shape options described here. |
| `DISCUSSION-ha-scale-and-offload.md` | Future `DESIGN-ha-v09x.md` (~0.9.x) | Parked. Post-1.0 territory. No action. |

**No promotions, no archives.** All four stay in `discussion/`. Re-visit at `RE-EVAL following 0.3.3` (per cadence) or if the 0.3.x arch session surfaces a constraint from the streaming doc.

### 5.4 `discussion/` parked-content watchdog

Once Plinth accumulates enough discussion docs to make churn tracking non-trivial (perhaps around 0.6.x), consider adding a "parked age" line to each discussion doc's frontmatter so the re-eval loop can mechanically list the oldest-parked files. Not a roadmap item yet — note for future methodology refinement.

---

## 6. Observations for methodology / future re-evals

### 6.1 The "caller-triggered implementation" pattern

the maintainer's prompt asked to watch for 0.2.6-style deferrals — *"Deferred because no caller exists yet"*. I found **six** clear instances, enough to call this a pattern rather than an outlier:

1. **0.2.6 async dispatch wrapper.** Full `Task<>` interface specified in ICD-0.2.2; sync body ships until the first coroutine caller appears (expected alongside 0.3.3 QuickJS async bridge).
2. **ICD-0.1.3 `last_used_at` write path.** Direct UPDATE vs. debounced write "will be decided when the realtime coalescer (0.5.1) is available; the chosen approach must remain compatible with that design."
3. **ICD-0.1.6 Implementation Note #1 — PG LISTEN/NOTIFY reader.** Wire format + subscriber fan-out in place; *"a 0.5.0 reader can call the same `publish()` entry point once it exists"*.
4. **ICD-0.1.6 Implementation Note #2 — per-channel rule naming convention.** Subscribe is admin-only for 0.1.6; *"per-channel rule naming convention is deferred to a later ICD written alongside real channel producers"*.
5. **`resolution.hpp` deviation #2 — Tier 1 handler bodies.** Five kernel capabilities are registered with `{"not_implemented": "<signature>"}` stub handlers; *"Real wiring lands when a real caller first demands it."*
6. **`batch.hpp` deviation #1 — sequential dispatch.** Interface is shaped for `Promise.all`-style concurrent dispatch; sequential body ships because *"the kernel has no thread pool today and no caller demanding parallelism — will be revisited alongside 0.2.6 or 0.3.3."*

**What all six share:**
- The **interface is committed to in the ICD or architecture**.
- The **body is a stub or the simplest-correct implementation** that passes its tests.
- The **trigger for filling out the body is named** (a specific future caller, a specific future milestone).
- The **deviation is documented** at the code site and in CHANGELOG — but usually not in the ICD itself (→ gap §2.x, repeatedly).

**This is distinct from YAGNI** (which would omit the interface) and from **speculative abstraction** (which would add machinery without a consumer). It's a third mode: *interface-first, body-gated-on-caller.* The methodology doesn't currently name it.

**Suggestion for a future methodology patch:**
- Give the pattern a name (working title: **caller-triggered implementation**, or **interface-first stubs**).
- Pair it with a lightweight discipline requirement: *when a deviation is taken in code, the owning ICD must get an "Implementation deviation" subsection in the same PR.* This would have prevented §§2.1–2.6 from occurring. The discipline is cheap — a few lines per PR — and it closes a class of interface-drift that the code-aware re-eval would otherwise have to rediscover every cycle.
- The ICD-0.1.6 *Implementation Notes* footer is already a working template for this subsection.

Not a roadmap item (this is architect-session material, not code). Flagged for the maintainer to decide whether to incorporate as a small METHODOLOGY patch at a future architecture session. If incorporated, update METHODOLOGY §Phase 2 or §Phase 3 with the discipline rule; the naming itself can live in the §Roadmap Milestone Labels section.

### 6.2 Re-eval scope will grow

This re-eval read essentially all of `src/` + all of `docs/`. That is feasible today because the codebase is ~54 files and ~10 ICDs. By 0.6.x, the kernel will be several times larger and the shell extension + admin extension will add more. METHODOLOGY §3.3 Project-lifetime implications already flags this; worth confirming at the 0.3.3 re-eval whether the "full sweep" approach is still workable or whether some kind of rotation (per-subsystem focus per re-eval) is warranted.

### 6.3 Zero-band-change outcome is legitimate for a first-cadence re-eval

METHODOLOGY §3.1 warns *"'Nothing needs to change' usually means we didn't look hard enough."* For this session specifically — five days after an exhaustive band-labeling session and before any new ICDs — zero band changes is the correct structural outcome. The code-aware half carries the substantive output. Worth noting in methodology guidance that *first-cadence re-evals after a bulk-label pass are expected to be structurally light.*

---

## 7. Exit criteria

Per METHODOLOGY §3.3:

- [x] Architecture document and roadmap updated per §3.1's questions.
- [x] Band promotions / demotions applied (none this cycle; rationale in §5.1).
- [x] Roadmap milestone labels reviewed; three new items added.
- [x] New discussion docs written? None surfaced from gap analysis.
- [x] Session output committed to the documentation tree (`docs/reviews/RE-EVAL-0.2.x.md` — this file).
- [x] Code untouched. No C++ edits.
- [x] CHANGELOG entry for the rewrite session.

---

## 8. Next actions for the architect

1. **Review this artifact.** Flag any gap disposition you disagree with.
2. **Decide on the three new roadmap items** — numbering, ordering, band assignments. My suggestions above are defaults, not decisions.
3. **Decide whether §6.1 (caller-triggered-implementation pattern + discipline rule) becomes a METHODOLOGY patch.** If yes, schedule as its own architect session; if no, the observation still lives in this artifact for future re-eval context.
4. **Schedule the architecture session** for ICDs 0.3.0 / 0.3.1 / 0.3.2 (new item A). This is Scale 3 per `architecture/02-capabilities.md §3`. It precedes any 0.3.x code work.
5. **Next re-eval:** `RE-EVAL following 0.3.3`. The cadence continues to fire every 4 code milestones.
