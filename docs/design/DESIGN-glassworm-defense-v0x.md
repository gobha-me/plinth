# DESIGN: GlassWorm Unicode Defense (0.4.1 + 0.6.x)

**Status:** Draft — v1 for architect review
**Scale:** 3 — Kernel security primitive (every extension receives the protection without opting in)
**Milestone:** 0.4.1 (scanner primitive + Layer 1 + Layer 2); Layer 3 deferred to 0.6.x
**Author:** the maintainer (Architect) + Claude (Architecture Session)
**Decision date:** 2026-04-19
**Traces to:** architecture/05-extensions.md §1.1 (Cross-File Manifest Validation — Layer 1 composes upstream of 0.4.2 in the `plinth validate` pass order); architecture/05-extensions.md §3 (QuickJS Runtime — Layer 2 composes with the hardening surface from 0.3.5); architecture/06-frontend.md (Frontend Architecture — Layer 3 composes with the shell's rendering surface, deferred)
**Depends on:**
- ICD-0.4.0-package-structure-validation (the `validate()` + `Reporter` surface that Layer 1 extends)
- ICD-0.3.5-runtime-hardening (the existing QuickJS execution gates that Layer 2 extends; specifically the `eval`/`Function` deletion precedent at `stdlib_inject.cpp`)
- ICD-0.3.1-runtime-lifecycle (the `JS_Eval` call-site enumeration at `eval.cpp:277`, `runtime_pool.cpp:697`, `run_on_context.cpp:816`)

**Informs:**
- Every future ICD that adds a new `JS_Eval` call site — the site owner must invoke the Layer 2 scan helper.
- DESIGN-shell-v06x.md — Layer 3 sanitization belongs to the shell's content-render path (deferred, 0.6.x).
- `Descusion GlassWorm.md` (root-level discussion file, 2026-04-19 input) — this doc absorbs and supersedes.

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

---

## 1. Decision

Plinth adds a kernel security primitive — a Unicode invisible-character scanner — that gates every source-text crossing the kernel's trust boundary. The scanner is hooked at three layers: package install (reject above threshold), QuickJS source load (fail-fast before `JS_Eval`), and frontend content render (strip/escape on display). The primitive is opt-out at the system level (a single config flag disables all three hooks); there is no per-extension allowlist and no per-request override. Every extension receives the protection without participating in it.

This decision commits Plinth to a proactive defense against the GlassWorm attack family (Oct 2025–present) and structurally adjacent supply-chain vectors that smuggle executable content through invisible Unicode. The decision is a one-way door: once the scanner is load-bearing, removing it requires architect-level approval because historical packages will have passed through it.

---

## 2. Why This Exists

GlassWorm is a supply-chain attack vector observed in the wild since October 2025. The attacker hides malicious JavaScript inside invisible Unicode characters — variation selectors, bidi overrides, zero-width spaces — that render as whitespace (or not at all) in most editors but are part of the source string passed to `eval()` / `Function()` / dynamic import at runtime. A package that appears clean on visual inspection can execute code that is not in the rendered source.

**Plinth-specific exposure.** Armature (Plinth's predecessor) ran Starlark — a deliberately small language without `eval`. Plinth runs QuickJS (ES2023), which exposes:

- `eval(string)` — direct source execution
- `new Function(string, ...)` — dynamic function construction
- Dynamic imports — `import(string)` (currently gated by 0.3.5's `eval`/`Function` deletion, but the broader string-to-execution surface is larger than those two globals)

These are the exact primitives GlassWorm exploits. The 0.3.5 hardening surface already deletes `globalThis.eval` and `globalThis.Function` (per ICD-0.3.5 §Security Constraint 5) — that closes the in-JS-runtime-level attack but does not close the upstream source-file attack: a package's `server/handlers/shell.js` can still carry invisible payloads that get fed to `JS_Eval` at extension load time. The C++ `JS_Eval` call site runs the host-submitted source verbatim, and whatever the source decodes to is what gets executed.

**Three-layer exposure.** The attack lands at three distinct entry points:

1. **Package install.** An attacker publishes a package to a registry (future) or tricks an admin into installing a locally-built package. The package's `.js` files carry invisible payloads. At install, the kernel runs the validator but does not inspect source bytes beyond structural rules.
2. **QuickJS source load.** Even with perfect install-time validation, a package upgrade can replace clean files with dirty ones (if upgrade validation is bypassed or the attacker compromises the upgrade path). Every `JS_Eval` call is a re-entry point.
3. **Frontend content render.** Extension-provided strings (panel titles, capability descriptions, admin notes) render into the shell's DOM. A dirty string rendered innerHTML-style can inject payloads that execute when a user with different locale settings (or a screen reader) triggers the browser's Unicode normalization.

Closing only one layer is insufficient. The scanner is the single primitive; the three hooks are the composition.

**What the discussion input contributed** (`Descusion GlassWorm.md`): the threat model summary, the initial Unicode range list, the threshold-based verdict rationale ("1-2 variation selectors = legitimate emoji; 50+ = suspicious"), and the three-layer decomposition. This doc preserves that framing and adds the architectural details (integration points, library surface, milestone decomposition, out-of-scope list) the discussion deferred.

---

## 3. The Unicode Scanner Primitive

One shared primitive. All three layers use the same implementation; they differ only in policy (what they do with the findings).

### 3.1 Scanner contract

```cpp
// src/kernel/security/unicode_scanner.hpp — NEW in 0.4.1
namespace plinth::security {

struct UnicodeFinding {
    std::size_t byte_offset;       // offset in UTF-8 source
    std::uint32_t codepoint;       // decoded codepoint
    std::string_view range_name;   // e.g. "variation-selector", "bidi-override"
};

struct UnicodeScanResult {
    std::vector<UnicodeFinding> first_findings;  // bounded (default: 5)
    std::size_t total_count = 0;
    bool exceeds_threshold = false;
    std::optional<std::string> decode_error;     // populated on malformed UTF-8
};

struct UnicodeScanConfig {
    std::size_t threshold = 50;                  // findings above this = suspicious
    std::size_t record_first_n = 5;              // bound on first_findings size
    bool strict_utf8 = true;                     // malformed UTF-8 = reject (not skip)
};

auto scan_for_invisible_unicode(std::string_view source,
                                const UnicodeScanConfig& cfg = {})
    -> UnicodeScanResult;

}  // namespace plinth::security
```

### 3.2 Scanned Unicode ranges

Comprehensive, not minimal. The scanner's value is in covering the full family of invisible / direction-control characters that a source-text-executing interpreter might render visually-safe while executing something else.

| Range | Name | Reason for inclusion |
|---|---|---|
| U+FE00–U+FE0F | Variation selectors | Primary GlassWorm payload encoding |
| U+E0100–U+E01EF | Variation selector supplements | Extended GlassWorm range |
| U+200B | Zero-width space | Classic tokenization bypass |
| U+200C | Zero-width non-joiner | Invisible in most fonts |
| U+200D | Zero-width joiner | Invisible in most fonts |
| U+200E | Left-to-right mark | Bidi primitive; harmless alone, suspicious in bulk |
| U+200F | Right-to-left mark | Bidi primitive; same rationale |
| U+202A–U+202E | Bidi override characters | "Trojan Source" class of attacks (2021 CVE-2021-42574) |
| U+2060 | Word joiner | Tokenization bypass |
| U+2061–U+2064 | Invisible math operators | Rarely legitimate in source; high signal |
| U+FEFF | Zero-width no-break space / BOM | Legitimate as file-leading BOM; suspicious mid-source |
| U+E0001 | Language tag | Rare; common smuggling target |
| U+E0020–U+E007F | Tag characters | Extended smuggling range |

**Threshold semantics.** A single variation selector after a base emoji codepoint is legitimate (e.g., `U+1F44D U+FE0F` renders the thumbs-up as an emoji rather than a pictogram). The scanner counts every member of the scanned ranges, including legitimate emoji variation selectors; the threshold separates "expected mild presence" from "smuggling-scale payload." Default threshold = 50. This value is tunable (§5) but the default is chosen to be well above any plausible legitimate-emoji density in a typical `.js` or JSON file.

**UTF-8 decode policy.** `strict_utf8 = true` (default). Malformed UTF-8 populates `decode_error` with a byte-offset + malformed-sequence description and returns a `UnicodeScanResult` with `exceeds_threshold = true` (treat malformed encoding as a scanner failure, and scanner failures block). Rationale: malformed UTF-8 in package source is already a red flag, and a scanner that silently skips malformed input would be defeatable by splicing an invalid byte into the source.

**Finding record.** For debuggability, the first `record_first_n` findings (default 5) are captured in `first_findings`. Subsequent findings contribute to `total_count` but are not individually recorded. This bounds memory under adversarial input (a file of pure variation selectors does not build a million-entry vector).

### 3.3 Performance budget

The scanner is on every package install and every `JS_Eval`. It must not be the bottleneck.

- **Target:** 100 MB/s throughput on the v0.4.0 CI builder image (single thread). A typical extension handler file (~10 KB) scans in < 1 µs; a 50 MB package-size-cap worst case scans in < 500 ms.
- **Implementation:** single-pass UTF-8 decoder, no allocations on the clean path, early-exit on threshold crossing.
- **Verification:** Google Benchmark harness under `benchmarks/unicode_scanner_benchmark.cpp` (new file, following the 0.2.6.2 pattern). Gated behind `PLINTH_BENCHMARKS=ON`. Minimum: one benchmark per Unicode range plus a clean-ASCII baseline.

### 3.4 Greenfield implementation

No existing UTF-8 decoder lives in `src/kernel/`. The scanner is the first UTF-8-aware module in the kernel; subsequent work (e.g. capability signature parsing of non-ASCII names, if ever) can reuse its decoder. The decoder is internal to `unicode_scanner.cpp` and not exposed; if a future caller needs UTF-8 decoding, that caller extracts the helper through an explicit ICD.

---

## 4. Integration Layers

Centralized. The scanner is one primitive; the three layers differ only in policy.

### 4.1 Layer 1 — Package install gate

**Injection point:** `src/kernel/packages/validator.cpp` at the `run_json_parse()` → `run_handler_files()` boundary (see the 0.4.0 implementation for exact line; today line 474 is the `validate()` entry, and the post-parse / pre-handler-walk boundary is the injection).

**What gets scanned:**
- Every regular file under `server/handlers/` with a `.js` suffix
- `server/main.js` (entry_point)
- Every regular file under `client/panels/` and `client/components/` with a `.js` or `.css` or `.html` suffix
- `manifest.json`, `capabilities.json`, `rbac.json`, `panels.json`, `config.json` — scanned as strings (a GlassWorm-style payload in a JSON string literal could reach the capability registry through `description` fields or similar)
- **Not scanned:** binary assets (images, fonts — identified by extension against a static allowlist), `migrations/*.sql` (PG source, not JS — out of scope for GlassWorm but flagged for future consideration), `README.md` / `LICENSE` / `CHANGELOG.md` (documentation; low-signal for GlassWorm). The allowlist is explicit in the implementation, not inverse of the scanned list.

**Policy:** Hard reject above threshold. New `ValidationMessage` rule `unicode-smuggle` with `Severity::ERROR`. The finding includes the file path, total count, and the first-five `UnicodeFinding` offsets for debuggability:

```
error: unicode-smuggle: server/handlers/shell.js contains 127 invisible Unicode characters (threshold 50); first findings at offsets 42, 43, 44, 45, 46 — range 'variation-selector'
  hint: inspect the file with `iconv -f utf-8 -t ascii//TRANSLIT` or a Unicode-aware hex dump; legitimate emoji should not trigger this rule
```

Pass ordering: Layer 1 runs AFTER 0.4.0's R1–R6 file-structural rules (so we don't scan a file that doesn't exist or fails R2 symlink checks) and BEFORE 0.4.2's cross-file rules (so a package with smuggled Unicode fails the cheap Unicode scan before we do whole-package cross-referencing).

**Report disposition impact:** any Layer 1 finding sets the overall `ValidationReport` disposition to 1 (error). Pass-with-warnings (disposition 2) is impossible with a Layer 1 finding.

### 4.2 Layer 2 — QuickJS source-load gate

**Injection points (three sites per ICD-0.3.1 surface):**
- `src/kernel/js/eval.cpp` — the one-shot `JS_Eval` at the file's single entry (today line 277 per the 0.4.0 code survey)
- `src/kernel/js/runtime_pool.cpp` — `eval_on_context` pooled path (today line 697)
- `src/kernel/js/run_on_context.cpp` — async coroutine path (today line 816)

**Shared helper:** A new `pre_eval_scan(std::string_view src) -> std::optional<UnicodeScanResult>` in `src/kernel/js/eval_guard.{hpp,cpp}` (new TU). Each of the three call sites invokes it immediately before `JS_Eval`. On threshold-exceed, the helper returns the populated result; the call site does NOT invoke `JS_Eval` and instead propagates the failure.

```cpp
// Pseudo-code; real integration lives in ICD-0.4.1-glassworm-defense when authored
if (auto scan = pre_eval_scan(src); scan && scan->exceeds_threshold) {
    return std::unexpected(EvalError{
        .kind = EvalErrorKind::UNICODE_SMUGGLE_DETECTED,
        .message = format_unicode_finding_message(*scan),
    });
}
// ... existing JS_Eval call ...
```

**New `EvalErrorKind::UNICODE_SMUGGLE_DETECTED` variant.** Joins the existing set (`CPU_TIME_EXCEEDED`, `WALL_CLOCK_EXCEEDED`, etc. per 0.3.1 + 0.3.5). The classifier at `conversion.cpp`'s `classify_rejection` does not need to map this — the variant is set by the pre-eval path, not the post-eval path, so it never reaches classification. The async bridge's error-code mapping (`async.unicode_smuggle`) fires only if an `async_op` payload carries the kind (not expected for Layer 2, which blocks at the synchronous boundary before the async op is constructed).

**Pass ordering within Layer 2:** pre-eval scan runs before the 0.3.1 CPU-timer bracket (the scan is not CPU-budgeted; scanner throughput is deterministic from source size and the 100 MB/s budget at §3.3 makes the scan cheap vs a typical JS_Eval). Runs after the `stdlib_inject` setup (so the scan can't be fooled by a handler that rewrites its own source pre-scan — the scan is on the bytes that will be passed to `JS_Eval`, not on the bytes the runtime has processed).

**Relationship to 0.3.5's `eval`/`Function` deletion:** complementary, not redundant. 0.3.5 closes the in-runtime `eval("code")` call (JS code can't construct JS code). Layer 2 closes the pre-runtime `JS_Eval(host_submitted_source)` call (the host can't submit smuggled source). Both close adjacent attack surfaces.

### 4.3 Layer 3 — Frontend content sanitization (deferred to 0.6.x)

**Status:** designed here, deferred to 0.6.x for implementation. The shell does not exist at 0.4.1; there is no render surface to hook.

**Injection point (future):** Whichever shell-side function renders extension-provided strings into the DOM (extension name, description, panel titles, capability documentation, admin notes). Per `architecture/06-frontend.md`, this is the shell's `renderExtensionString(s)` or equivalent — to be named when DESIGN-shell-v06x.md is consulted during 0.6.3.

**Policy shift from Layers 1/2:** strip / escape rather than reject. Rationale: the Layer 1 and Layer 2 gates already reject smuggled packages and refuse to execute smuggled source. By the time a string reaches Layer 3, either the scan already happened upstream (and the string is clean) or the string came from a path Layer 1/2 didn't cover (e.g., an admin-provided note). The frontend's job is UX preservation — stripping the findings and rendering the visible content is better than showing a blank panel with no error.

The stripped-string API looks like:

```cpp
// Conceptual; the actual surface lands in 0.6.x's ICD
struct SanitizedString {
    std::string visible;              // with invisible Unicode stripped
    UnicodeScanResult scan_result;    // for audit logging
};

auto sanitize_for_render(std::string_view raw) -> SanitizedString;
```

**Pass ordering:** Layer 3 runs at render time, not at ingress. Strings stored in the database may carry Unicode (e.g., a user's display name contains legitimate emoji); stripping at ingress would damage legitimate content. Strip at the shell boundary only.

**0.6.x ICD obligation.** When DESIGN-shell-v06x.md consumes this DESIGN doc at 0.6.3, the shell ICD adds a Layer 3 integration section naming the scan helper and the render-time call sites. This DESIGN doc does not pre-author the shell ICD.

---

## 5. Configuration & Logging

### 5.1 Config schema

`config.yml` gains a `security.unicode_scanner` block (new top-level `security` key; future security primitives live under the same namespace):

```yaml
security:
  unicode_scanner:
    enabled: true           # default true; setting false disables all three layers
    threshold: 50           # finding count at or above which = rejection
    log_findings: true      # emit audit entries for every above-threshold event
```

Reading the config:

- `Config::parse` (the existing kernel config parser) gains three fields. Absent section = defaults above (secure-by-default).
- `BridgeContext::ConfigProjection` (the per-runtime value-copy from 0.3.2) gains the three scanner fields so JS-level bindings can read config through the existing surface if ever needed. Not consumed today; reserved for future policy hooks.

### 5.2 Audit logging

Every above-threshold finding emits an audit event via the existing `plinth::log::audit` path (from 0.1.7 + 0.3.4.1's shutdown gate). Event shape:

```json
{
  "event": "security.unicode_smuggle_detected",
  "layer": "install" | "eval" | "render",
  "source_path": "<file path or '<eval>' etc>",
  "total_count": 127,
  "first_findings": [
    {"byte_offset": 42, "codepoint": 65024, "range_name": "variation-selector"}
  ],
  "threshold": 50
}
```

Audit runs through the existing async Drogon writer; the shutdown gate from 0.3.4.1 (`plinth::log::shutdown`) applies unchanged.

**`log_findings: false` semantics.** Even with `log_findings` off, the gate still rejects (Layer 1 returns the `ValidationMessage`, Layer 2 returns the `EvalErrorKind::UNICODE_SMUGGLE_DETECTED`). The flag suppresses the audit-trail write only. Rationale: a site with extreme log-volume concerns can mute the audit without opening the security hole.

### 5.3 spdlog integration

Kernel-side info log (non-audit) on every finding: `spdlog::warn("unicode-smuggle detected: layer={} path={} count={} threshold={}", ...)`. Standard pattern; no new surface.

---

## 6. Per-Version Scope

### 6.1 — 0.4.1 (scanner + Layer 1 + Layer 2)

**Scope:**
- `src/kernel/security/unicode_scanner.{hpp,cpp}` — the primitive
- Layer 1 wiring in `validator.cpp` — new rule `unicode-smuggle`; pre-cross-file pass ordering
- Layer 2 wiring — `src/kernel/js/eval_guard.{hpp,cpp}` (new) + three call-site integrations at `eval.cpp:277`, `runtime_pool.cpp:697`, `run_on_context.cpp:816`
- `EvalErrorKind::UNICODE_SMUGGLE_DETECTED` variant
- Config schema additions in `config.yml` (kernel + client config)
- Audit event shape `security.unicode_smuggle_detected`
- Test fixtures: `tests/fixtures/packages/unicode-smuggle-variation-selectors/`, `tests/fixtures/packages/unicode-smuggle-bidi-override/`, `tests/fixtures/packages/unicode-smuggle-malformed-utf8/`, `tests/fixtures/packages/unicode-legitimate-emoji/` (baseline — passes through threshold)
- Catch2 cases: ≥ 10 (scanner unit tests covering every range + threshold boundary + malformed UTF-8; one per-layer integration test for Layers 1 and 2)
- Benchmark harness under `benchmarks/unicode_scanner_benchmark.cpp`

**Exit criteria:** All test fixtures produce the expected disposition; scanner benchmark meets the 100 MB/s budget on CI builder image; zero tidy findings; `ctest` pass count increases by the new cases; sanitizer suite clean on Layer 1 and Layer 2 integration tests.

### 6.2 — 0.6.x (Layer 3)

**Scope:** Shell-side integration at the render-time call sites. Authored as part of the DESIGN-shell-v06x.md consumption at 0.6.3.

**Deferred detail:** the strip-vs-escape policy decision for the frontend; the exact shell rendering surface; any CSP interaction.

### 6.3 Arc entry criteria

**0.4.1 entry:** 0.4.0 merged (validator + `Reporter` surface exists); 0.3.5 merged (QuickJS hardening surface exists with the `eval`/`Function` deletion precedent for pre-eval gating). Both shipped in the current `main` as of 2026-04-19 — no upstream blocker.

**0.6.x Layer 3 entry:** DESIGN-shell-v06x.md's render surface defined; 0.6.3 milestone active.

---

## 7. Out of Scope

Per `Descusion GlassWorm.md:113–118`, five capabilities are explicitly deferred. This DESIGN doc preserves the deferrals with reasoning:

- **Runtime detection in already-loaded scripts.** Once JS has been parsed into bytecode, the original source-text Unicode is no longer observable. Deferred indefinitely; the three-layer gate covers the ingress paths, and in-runtime detection would require extending the QuickJS heap with source-preservation metadata — not worth the complexity at this scale.
- **Content-Security-Policy integration.** CSP belongs to the shell's rendering surface (0.6.x). Layer 3 is the integration point. Architectural orthogonality: the scanner is about source bytes; CSP is about execution context. Keep them separate.
- **Administrative override mechanisms.** No per-package allowlist, no per-request override, no "I know what I'm doing" escape hatch. Rationale: an override is an attack vector. If an extension legitimately needs to include > 50 invisible Unicode characters, the path forward is raising the global threshold (§5.1) with architect approval, not a one-off bypass.
- **Allowlist for specific packages.** Same reasoning. The primitive is binary: either the threshold is met or it isn't. Providing a bypass provides an attack target.
- **Per-extension scanner tuning.** A malicious extension author could set a permissive per-extension threshold in their manifest and ship the attack. The threshold is kernel-owned, not extension-owned.

Additionally:

- **Scanning non-JS assets (images, binaries, fonts).** The scanner operates on UTF-8 source text. Binary assets are out of scope — they don't reach `JS_Eval` and the validator doesn't need to interpret them as source. The static allowlist at §4.1 excludes binaries explicitly.
- **Migration SQL scanning.** PG source can carry invisible Unicode too, but the attack surface is different (SQL is executed through libpq, not `JS_Eval`). Flagged for future consideration; not 0.4.1 scope.
- **Real-time log-stream scanning.** Audit logs can themselves carry Unicode smuggled through an extension's log-bindings calls. The log-bindings path already passes through the scanner at Layer 2 (the JS that constructs the log call ran through `JS_Eval`), so the risk is narrow. Deferred.

---

## 8. Open Questions

Resolve during 0.4.1 implementation or in the architect-review PR:

1. **Threshold default value.** 50 is a first-cut number from `Descusion GlassWorm.md`. Is the signal cleanly separable at 50, or do real legitimate-emoji documents (e.g. a 10 KB `.js` file with a large skin-tone-variation emoji table) trip the gate? Proposed: ship at 50; capture the first false-positive report as a datum; tune on evidence.
2. **BOM (U+FEFF) policy.** Leading BOM is legitimate as a file-start marker. Proposed: the scanner counts BOM in `total_count` only if it appears mid-source (offset > 2, accounting for the 3-byte UTF-8 BOM encoding). Alternative: always count BOM; a legitimate BOM contributes 1, well under threshold. Simpler = always-count; architect picks.
3. **Layer 2 position relative to call-depth check.** ICD-0.2.2's `MAX_CALL_DEPTH=8` enforcement runs in the capability dispatcher, not at `JS_Eval`. Layer 2's scan at `run_on_context.cpp:816` is inside the async coroutine path, which already has call-depth threaded through. Proposed: scan runs before call-depth check (the depth-chain test N.44 from 0.3.5 uses a clean source, so ordering doesn't affect existing tests). Document in the ICD.
4. **Audit event volume under pathological load.** An attacker repeatedly uploading smuggled packages generates audit-event spam. Proposed: rate-limit audit emission via a 1 Hz token bucket per `layer` + per `source_path`; emit a single `security.unicode_smuggle_rate_limited` event when the bucket overflows. Alternative: let the audit infrastructure handle it at a lower level. Architect decision — this design doc does not foreclose either.
5. **Unicode normalization before scan.** Unicode has NFC / NFD / NFKC / NFKD normalization forms. Should the scanner normalize to NFC before counting? Proposed: **no normalization.** The attack relies on the byte-level sequence, and normalization could fold a smuggled sequence into a clean one (or vice versa) unpredictably. Scan raw bytes. Flag in ICD as a deliberate choice.
6. **Scanner disabled-state auditing.** If an operator sets `security.unicode_scanner.enabled: false`, should the kernel emit a startup audit event `security.unicode_scanner_disabled` as a compliance breadcrumb? Proposed: yes — a disabled scanner is a policy deviation and the audit trail should record it. Low implementation cost.

---

## 9. Milestone Criteria

0.4.1 closes when all of the following are true:

1. **All 5 adversarial test cases from `Descusion GlassWorm.md:106–111` pass:**
   - Legitimate Unicode (emoji with 1-2 variation selectors) passes Layer 1 + Layer 2
   - GlassWorm signature (100+ hidden variation selectors) rejects at Layer 1 and Layer 2
   - Edge cases: exactly-at-threshold, mixed ranges, malformed UTF-8
   - Performance: large source files (50 MB) scan within the 100 MB/s budget
2. **Zero performance regression on Unicode-clean codepaths.** The benchmark harness shows < 5% overhead on the clean-ASCII baseline vs a pre-0.4.1 reference build.
3. **Clear error messages.** Both Layer 1 and Layer 2 rejection messages cite the `unicode-smuggle` rule by name, report the total count and threshold, and provide the first-five `UnicodeFinding` offsets. No generic "validation failed" messages.
4. **All findings logged.** Audit events land for every above-threshold detection across all layers, subject to `log_findings` flag and the rate-limit policy from §8.
5. **No GlassWorm payloads reach QuickJS `eval()` through any validated path.** Adversarial test: a fixture with 100 variation selectors in a handler file passes through the installer (rejected), through a direct `eval()` attempt (rejected), and through an upgrade replay (rejected at Layer 2 re-scan at load time).
6. **Legitimate extensions continue working.** The 0.4.0 `valid-full/` fixture and the shell's bundled blob (when 0.6.0 ships) pass through unchanged.
7. **Architect ratification of Open Questions §8.** At least the threshold default, BOM policy, and normalization-skip decisions are recorded in the ICD-0.4.1 CHANGELOG entry or PR body (matches the 0.3.5 "Open Questions tacit ratification" precedent).

On these conditions, the kernel security primitive is load-bearing and the Plinth attack surface against GlassWorm-class supply-chain attacks is covered at Layers 1 and 2. Layer 3 remains scheduled for 0.6.x.
