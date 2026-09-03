# ICD-0.4.1-glassworm-defense

**Traces to:** DESIGN-glassworm-defense-v0x.md §3 (Scanner Primitive), §4.1 (Layer 1 — Package Install Gate), §4.2 (Layer 2 — QuickJS Source-Load Gate), §5 (Configuration & Logging), §6.1 (Per-Version Scope — 0.4.1), §9 (Milestone Criteria); architecture/05-extensions.md §1 (Package Structure — Layer 1 scan domain); architecture/05-extensions.md §1.1 (Cross-File Manifest Validation — Layer 1 pass ordering vs 0.4.2); architecture/05-extensions.md §3.1 (QuickJS Runtime Limits — Layer 2 composition with 0.3.x hardening).
**Depends on:** ICD-0.4.0-package-structure-validation (`validate()` entry, `ValidationReport`, `ValidationMessage`, `Severity::ERROR` — Layer 1 extends, does not replace); ICD-0.3.5-runtime-hardening (`globalThis.eval` / `globalThis.Function` deletion at `inject_kernel_stdlib` — the in-runtime gate Layer 2 composes with); ICD-0.3.1-runtime-lifecycle (`EvalErrorKind` enum, the three `JS_Eval` call sites, `plinth_js_interrupt_cb`); ICD-0.3.3-async-bridge (`BridgeContext` + `ConfigProjection` field-addition pattern).
**Milestone:** 0.4.1 — GlassWorm Unicode defense layer. Kernel security primitive (`plinth::security::UnicodeScanner`) plus two integration layers: package install gate (Layer 1) and QuickJS source-load gate (Layer 2). Layer 3 (frontend content sanitization) is designed in DESIGN §4.3 and deferred to 0.6.x.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** `src/kernel/packages/validator.cpp:474` (Layer 1 injection — the `validate()` entry function; post-parse / pre-handler-walk boundary is the specific hook); `src/kernel/js/eval.cpp:277`, `src/kernel/js/runtime_pool.cpp:756`, `src/kernel/js/run_on_context.cpp:816` (the three `JS_Eval` call sites Layer 2 hooks — line numbers verified on current `main` as of 2026-04-19; DESIGN-glassworm-defense-v0x.md §4.2 cites `runtime_pool.cpp:697`, which drifted after 0.4.0.1's `drain_pending_jobs` landed — this ICD carries the corrected line number); `Descusion GlassWorm.md` (repo-root architect input, 2026-04-19 — threat-model framing absorbed into DESIGN §2); `docs/reviews/RE-EVAL-0.4.x.md §2.5, §6, §11` (mid-cadence absorption, forward-ICD authoring obligation, Open Question ratification requirement); DEFERRED.md (no active entries block 0.4.1).

---

## Overview

GlassWorm (observed Oct 2025–present) smuggles executable JavaScript inside invisible Unicode ranges — variation selectors, bidi overrides, zero-width characters — that render as whitespace in editors but reach `eval`-family execution surfaces verbatim. Plinth runs QuickJS (ES2023) and exposes `JS_Eval` as the kernel-to-runtime source-submission primitive: the C++ host passes UTF-8 source bytes in, QuickJS parses and executes them. 0.3.5 closed the in-runtime `eval` / `Function` surface (`globalThis.eval` deleted at `inject_kernel_stdlib`), but the upstream `JS_Eval` entry — which runs before any in-runtime gate — remained open. 0.4.0's `plinth validate` inspects package structure but does not inspect source bytes for encoding-level attacks. Both entries are the attack surface this milestone closes.

0.4.1 introduces one primitive and two policy layers:

- **Primitive** — `plinth::security::UnicodeScanner::scan_for_invisible_unicode()` — a single-pass UTF-8 decoder with a threshold-based verdict. One implementation, re-used at every gate.
- **Layer 1** — wired into `plinth::packages::validate()`; above-threshold findings land as a new `unicode-smuggle` rule with `Severity::ERROR`, setting `ValidationReport::disposition` to 1 (error). Composes *after* 0.4.0's R1–R6 structural rules and *before* 0.4.2's cross-file rules.
- **Layer 2** — new `src/kernel/js/eval_guard.{hpp,cpp}` translation unit hosting `pre_eval_scan()`; called at each of the three `JS_Eval` call sites before the runtime receives the source. Above-threshold findings return `std::unexpected(EvalError{kind: UNICODE_SMUGGLE_DETECTED})`. New enum variant `EvalErrorKind::UNICODE_SMUGGLE_DETECTED` in `eval.hpp`.

Layer 3 (frontend content sanitization) is designed in DESIGN §4.3 and deferred to 0.6.x — Plinth's frontend shell does not exist at 0.4.1.

**Scope:**

- New `src/kernel/security/unicode_scanner.{hpp,cpp}` translation unit — the primitive. First UTF-8-aware module in the kernel (greenfield).
- New `src/kernel/js/eval_guard.{hpp,cpp}` translation unit — Layer 2 pre-eval helper.
- Modification to `src/kernel/packages/validator.cpp` — add `run_unicode_scan_pass()` invoked from `validate()` after `run_json_parse()` and before `run_handler_files()`. New `unicode-smuggle` rule.
- Modifications to `src/kernel/js/eval.cpp`, `src/kernel/js/runtime_pool.cpp`, `src/kernel/js/run_on_context.cpp` — three two-line integrations (call `pre_eval_scan` before `JS_Eval`, propagate the `std::unexpected` on threshold exceed).
- New `EvalErrorKind::UNICODE_SMUGGLE_DETECTED` variant in `eval.hpp`.
- Config schema additions — `security.unicode_scanner.{enabled, threshold, log_findings}`. `Config::parse` gains three fields; `BridgeContext::ConfigProjection` gains three reserved fields (not consumed in 0.4.1 bindings; wired for future policy hooks).
- Audit events — `security.unicode_smuggle_detected`, `security.unicode_smuggle_rate_limited`, `security.unicode_scanner_disabled`. All through the existing `plinth::log::audit` path (0.3.4.1 shutdown-gate applies unchanged).
- Test fixtures — four new package directories under `tests/fixtures/packages/`: `unicode-smuggle-variation-selectors/`, `unicode-smuggle-bidi-override/`, `unicode-smuggle-malformed-utf8/`, `unicode-legitimate-emoji/`.
- Catch2 test battery — ≥13 cases distributed across two new files (`tests/kernel/security/unicode_scanner_test.cpp`, `tests/kernel/js/eval_guard_test.cpp`) and the existing `tests/kernel/packages/validator_test.cpp`.
- Benchmark harness — `benchmarks/unicode_scanner_benchmark.cpp`, gated behind `PLINTH_BENCHMARKS=ON` (follows the 0.2.6.2 pattern). Acceptance: 100 MB/s throughput single-thread on the v0.4.0 CI builder image.

**Out of scope (deferred):**

- **Layer 3 (frontend content sanitization).** Designed in DESIGN §4.3; implementation in 0.6.x. This ICD does not pre-author the shell-side surface.
- **Migration SQL scanning.** `migrations/*.sql` files reach libpq, not `JS_Eval`. Deferred; flagged in DESIGN §7 as future consideration.
- **Runtime detection in already-loaded scripts.** Once source has been parsed to bytecode, the original UTF-8 bytes are gone. Indefinite deferral per DESIGN §7.
- **CSP integration.** Belongs to Layer 3 / shell rendering; not 0.4.1 scope.
- **Administrative override / per-package allowlist / per-request bypass.** Refused per DESIGN §7 ("overrides are attack vectors"). No escape hatch. The only knob is the global `security.unicode_scanner.enabled` flag.
- **Per-extension scanner tuning.** Threshold is kernel-owned, not extension-owned, per DESIGN §7 rationale.
- **Scanning non-JS binary assets.** Images, fonts, binaries are skipped by the Layer 1 file-extension allowlist (§4.1 below). Binary content does not reach `JS_Eval`.
- **Unicode normalization (NFC / NFD / NFKC / NFKD) before scan.** Scan operates on raw UTF-8 bytes per DESIGN §8 Resolved Open Question #5 — normalization could fold a smuggled sequence into or out of the scan ranges unpredictably.

---

## Scanner Primitive

One implementation, re-used at every gate. Policy differs by layer; the primitive does not know who called it.

### Public contract

```cpp
// src/kernel/security/unicode_scanner.hpp — NEW in 0.4.1
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::security {

struct UnicodeFinding {
    std::size_t byte_offset;       // offset in UTF-8 source
    std::uint32_t codepoint;       // decoded codepoint
    std::string_view range_name;   // e.g. "variation-selector", "bidi-override"
};

struct UnicodeScanResult {
    std::vector<UnicodeFinding> first_findings;  // bounded by cfg.record_first_n
    std::size_t total_count = 0;
    bool exceeds_threshold = false;
    std::optional<std::string> decode_error;     // populated on malformed UTF-8
};

struct UnicodeScanConfig {
    std::size_t threshold = 50;                  // findings at or above this = rejection
    std::size_t record_first_n = 5;              // upper bound on first_findings.size()
    bool strict_utf8 = true;                     // malformed UTF-8 = reject (not skip)
};

auto scan_for_invisible_unicode(std::string_view source,
                                const UnicodeScanConfig& cfg = {})
    -> UnicodeScanResult;

}  // namespace plinth::security
```

The function is pure (no kernel state, no allocations on the clean path). `first_findings.size() <= cfg.record_first_n` is invariant; `total_count` is the authoritative count (may exceed `first_findings.size()`); `exceeds_threshold = (total_count >= cfg.threshold) || decode_error.has_value()`.

The `range_name` field is a `string_view` into a static table; no lifetime concern at the call site. Range names are a fixed vocabulary defined in §Scanned Ranges below.

### Scanned ranges

Comprehensive. The scanner counts every member of the enumerated ranges; separation between legitimate and smuggled input is by count (threshold), not by codepoint identity.

| Range | Name (`range_name`) | Rationale |
|---|---|---|
| U+FE00–U+FE0F | `variation-selector` | Primary GlassWorm payload encoding |
| U+E0100–U+E01EF | `variation-selector-supplement` | Extended GlassWorm range |
| U+200B | `zero-width-space` | Classic tokenization bypass |
| U+200C | `zero-width-non-joiner` | Invisible in most fonts |
| U+200D | `zero-width-joiner` | Invisible in most fonts |
| U+200E | `ltr-mark` | Bidi primitive; harmless alone, suspicious in bulk |
| U+200F | `rtl-mark` | Bidi primitive |
| U+202A–U+202E | `bidi-override` | "Trojan Source" attacks (CVE-2021-42574) |
| U+2060 | `word-joiner` | Tokenization bypass |
| U+2061–U+2064 | `invisible-math-operator` | Rarely legitimate in source |
| U+FEFF | `bom` | Leading BOM legitimate; counted anywhere per §Resolved Open Questions #2 |
| U+E0001 | `language-tag` | Rare; common smuggling target |
| U+E0020–U+E007F | `tag-character` | Extended smuggling range |

Any codepoint outside these ranges is ignored.

### UTF-8 decode policy

`strict_utf8 = true` (default). Malformed UTF-8 populates `decode_error` with a byte-offset + short description (e.g. `"invalid continuation byte at offset 42"`) and sets `exceeds_threshold = true`. Rationale: malformed UTF-8 in package source is itself a red flag, and a scanner that silently skipped malformed bytes would be defeatable by splicing invalid bytes into the smuggled payload.

On `decode_error`, the function returns immediately — `total_count` may be partial (count of valid findings before the malformed byte). Call sites should treat any `decode_error.has_value()` result as rejection regardless of `total_count`.

### Implementation latitude

- Single-pass UTF-8 decoder, internal to `unicode_scanner.cpp`, not exposed. If a future kernel caller needs UTF-8 decoding as its own primitive, the extraction happens through a new ICD — 0.4.1 keeps the decoder a private implementation detail.
- No allocations on the clean path. `first_findings.reserve(cfg.record_first_n)` is acceptable (bounded); beyond that, no heap growth.
- Early-exit on threshold crossing is permitted *as an optimization* but is NOT the observable contract — callers may rely on `total_count` being the full count. If early-exit is implemented, it must continue counting to a caller-observable bound; the recommended semantics: continue counting until `total_count == cfg.threshold * 2` (gives the caller enough information for audit without running the full file in the worst case). Implementation choice; test coverage pins behavior at the boundary.

### Performance budget

Acceptance target: **100 MB/s single-thread** on the v0.4.0 CI builder image (Ubuntu 25.10 + clang-20). A typical 10 KB handler scans in < 1 µs; the 50 MB `maximum package size` default (architecture/05-extensions.md §3.1) scans in < 500 ms. Verified by the benchmark harness at `benchmarks/unicode_scanner_benchmark.cpp` (gated `PLINTH_BENCHMARKS=ON`, following 0.2.6.2 convention).

Clean-ASCII baseline: scanner overhead vs a no-op pass through the bytes must be < 5% on the same builder image. Measured via the same harness with a 1 MiB ASCII payload.

---

## Layer 1 — Package-Install Gate

### Injection point

`src/kernel/packages/validator.cpp`, inside `plinth::packages::validate()` (line 474 on current `main`). The new pass `run_unicode_scan_pass()` runs between `run_json_parse()` and `run_handler_files()`:

```cpp
// validator.cpp:474 — after 0.4.1 wiring
auto validate(const fs::path& package_root, const ValidationConfig& cfg)
    -> ValidationReport {
    ValidationReport report;
    Reporter r{.out = &report.messages};
    if (!run_input_preflight(package_root, r)) return report;

    run_required_files(package_root, r);
    run_walk(package_root, cfg, r, report.files_scanned, report.total_bytes);
    auto parsed = run_json_parse(package_root, r);
    run_unicode_scan_pass(package_root, cfg, r);   // NEW in 0.4.1
    run_handler_files(package_root, parsed, r);
    run_panel_files(package_root, parsed, r);

    return report;
}
```

Pass-ordering invariants:

- **After R1–R6 (0.4.0 structural rules):** run_walk / run_required_files must succeed enough to know that a file exists and is a regular file inside the package root. Scanning a path that failed R2 (symlink-outside) or R1 (missing required file) is meaningless.
- **After run_json_parse:** JSON parsers already enforce structural soundness; the scanner reads raw file bytes (not parsed JSON string values), so order is insensitive here, but placing it after is cleaner for test sequencing.
- **Before cross-file rules (0.4.2):** cross-file walks `capabilities.json` / `rbac.json` — a package with a smuggled payload fails the cheap per-file scan before the whole-package cross-reference traversal runs. 0.4.2's `run_cross_file_validation()` is not yet shipped but the ordering is documented here so 0.4.2's implementer inserts their pass after `run_unicode_scan_pass` rather than before.

### Files scanned

Explicit allowlist — implementation must not derive the scan list by negating a binary-asset list (inversion is fragile).

| Path prefix | Extensions |
|---|---|
| `server/handlers/` | `.js` |
| `server/` (top-level only) | `main.js` (the `entry_point`) |
| `client/panels/` | `.js`, `.css`, `.html` |
| `client/components/` | `.js`, `.css`, `.html` |
| (root) | `manifest.json`, `capabilities.json`, `rbac.json`, `panels.json`, `config.json` |

All five root JSON files are scanned as raw UTF-8 bytes. A smuggled payload inside a JSON string literal (e.g. a `description` field) is structurally valid JSON but the bytes still decode through `JS_Eval` when the capability registry or panel surface later stringifies the field — Layer 1 catches this at install, before any downstream stringification.

**Not scanned:**

- `migrations/*.sql` (PG source; different execution surface; out of scope per §Out of Scope)
- `README.md` / `LICENSE` / `CHANGELOG.md` (documentation; low-signal)
- Binary asset extensions (`.png`, `.jpg`, `.jpeg`, `.gif`, `.ico`, `.svg`, `.woff`, `.woff2`, `.ttf`, `.otf`, `.eot`, `.webp`, `.avif`, `.mp3`, `.mp4`, `.webm`, `.pdf`, `.zip`) — explicit static list; unknown extensions inside `client/panels/` or `client/components/` that are not on the scan-extension list *and* not on the binary-extension list trigger an informational trace `{rule: "unicode-scan-skipped-unknown-ext", severity: note}` but do not fail validation. The `note` severity is below `warning`; it surfaces only in `--json` output.
- Symlinks — already rejected by R2 (0.4.0) before this pass runs.

The scan pass reads each file in one `std::ifstream` / `std::filesystem` read; for the 50 MB package-size cap the worst-case total read is bounded. No memory-mapping; no parallelism.

### Rule name and emitted message

Rule name: **`unicode-smuggle`**. Severity: `Severity::ERROR`. A Layer 1 finding sets `ValidationReport::disposition` to 1 (error); pass-with-warnings (disposition 2) is not reachable with any Layer 1 finding.

Canonical error-text shape:

```
error: unicode-smuggle: server/handlers/shell.js contains 127 invisible Unicode characters
       (threshold 50); first findings at offsets 42, 43, 44, 45, 46 — range 'variation-selector'
  hint: inspect with `iconv -f utf-8 -t ascii//TRANSLIT` or a Unicode-aware hex dump;
        legitimate emoji rarely contribute more than a handful of findings.
```

If `decode_error` is populated (malformed UTF-8), the message variant is:

```
error: unicode-smuggle: server/handlers/shell.js failed UTF-8 decode at offset 42
       (invalid continuation byte)
  hint: the package source is not valid UTF-8. Re-export from your editor with UTF-8
        encoding, or inspect the file at the cited offset.
```

The `ValidationMessage` struct populates:

- `path` = package-root-relative file path (e.g. `server/handlers/shell.js`)
- `rule` = `"unicode-smuggle"`
- `message` = the canonical text above
- `remediation` = the `hint:` body (extracted separately for `--json` consumers)

The `--json` output includes an additional `unicode_smuggle` object on each finding:

```json
{
  "severity": "error",
  "path": "server/handlers/shell.js",
  "rule": "unicode-smuggle",
  "message": "...",
  "remediation": "...",
  "unicode_smuggle": {
    "total_count": 127,
    "threshold": 50,
    "first_findings": [
      {"byte_offset": 42, "codepoint": 65024, "range_name": "variation-selector"},
      {"byte_offset": 43, "codepoint": 65024, "range_name": "variation-selector"}
    ]
  }
}
```

The `unicode_smuggle` block is optional on non-`unicode-smuggle` messages (absent). This extension is additive; legacy `--json` consumers ignore unknown fields.

### CLI impact

No new `plinth validate` flag. Layer 1 runs unconditionally whenever `security.unicode_scanner.enabled == true` (the default). When disabled at the config level, `run_unicode_scan_pass()` is a no-op; the CLI still runs, `ValidationReport` reflects zero Layer 1 findings. The `security.unicode_scanner_disabled` audit event (§Audit) fires once at kernel startup if disabled — the CLI `plinth validate` invocation does not re-emit; validation is a CLI process, not a kernel process, and a disabled scanner in a CLI context is an operator-inspection scenario, not a runtime-security scenario.

The existing `--max-size` / `--json` / `--quiet` flags are unchanged.

---

## Layer 2 — QuickJS Source-Load Gate

### Shared helper

New translation unit `src/kernel/js/eval_guard.{hpp,cpp}`. Public surface:

```cpp
// src/kernel/js/eval_guard.hpp — NEW in 0.4.1
#pragma once
#include <string_view>
#include "eval.hpp"                             // EvalError, EvalErrorKind
#include "../security/unicode_scanner.hpp"      // UnicodeScanResult

namespace plinth::js {

// Returns std::nullopt on clean source; populated EvalError on threshold exceed
// or UTF-8 decode failure. Caller propagates the unexpected to its return path.
auto pre_eval_scan(std::string_view src,
                   std::string_view source_label)  // "<eval>", "<pool>", etc.
    -> std::optional<EvalError>;

}  // namespace plinth::js
```

`source_label` identifies the call site for audit-event attribution and error-message formatting. Reads the scanner config via a file-scope `get_scanner_config()` helper (which in turn reads from the process-global `plinth::Config` loaded at startup — the scanner config is not per-BridgeContext because it's a security primitive with uniform system-wide policy).

### Integration points

Three two-line integrations. Each site calls `pre_eval_scan` immediately before `JS_Eval`:

| File | Line | Source label | Prior ICD |
|---|---:|---|---|
| `src/kernel/js/eval.cpp` | 277 | `"<eval>"` | ICD-0.3.1 one-shot `eval` path |
| `src/kernel/js/runtime_pool.cpp` | 756 | `"<pool>"` | ICD-0.3.1 `eval_on_context` pooled path |
| `src/kernel/js/run_on_context.cpp` | 816 | `"<async>"` | ICD-0.3.3 coroutine `eval_on_context_async` path |

DESIGN-glassworm-defense-v0x.md §4.2 and §6.1 cite `runtime_pool.cpp:697`; that reference drifted after 0.4.0.1's `drain_pending_jobs` helper landed (adding ~60 lines above the call site). The current `main` line is 756. Implementers should verify the line at implementation time via `grep -n 'JS_Eval(' src/kernel/js/runtime_pool.cpp` — the ICD's authoritative pointer is the function containing the `JS_Eval` call, not the line number.

### Failure-path skeleton (pseudocode)

```cpp
// eval.cpp — inside the one-shot eval function, immediately above line 277
if (auto err = pre_eval_scan(src, "<eval>"); err.has_value()) {
    return std::unexpected(std::move(*err));
}
JSValue ev = JS_Eval(ctx, src.data(), src.size(), "<eval>", ...);  // existing
```

```cpp
// runtime_pool.cpp — inside eval_on_context, immediately above line 756
if (auto err = pre_eval_scan(src, "<pool>"); err.has_value()) {
    return std::unexpected(std::move(*err));
}
JSValue ev = JS_Eval(bc.ctx, src.data(), src.size(), "<pool>", ...);  // existing
```

```cpp
// run_on_context.cpp — inside eval_on_context_async, immediately above line 816
if (auto err = pre_eval_scan(source, "<async>"); err.has_value()) {
    co_return std::unexpected(std::move(*err));
}
JSValue result = JS_Eval(bc.ctx, source.data(), source.size(), ...);  // existing
```

The async case uses `co_return` (not `return`) because the enclosing function is a coroutine (`drogon::Task<std::expected<...>>`). This matches the rest of `run_on_context.cpp`'s error-propagation style.

### New EvalErrorKind variant

`src/kernel/js/eval.hpp` gains one enum value:

```cpp
enum class EvalErrorKind {
    SYNTAX_ERROR,
    RUNTIME_ERROR,
    MEMORY_LIMIT,
    CPU_TIME_EXCEEDED,
    WALL_CLOCK_EXCEEDED,
    STACK_OVERFLOW,
    CANCELLED,
    PROMISE_REJECTED_UNHANDLED,
    ASYNC_RESULT_SIZE_EXCEEDED,
    UNICODE_SMUGGLE_DETECTED,       // new in 0.4.1
    INTERNAL,
};
```

`EvalError::message` carries the canonical text (first-findings, total count, threshold, source label) constructed by `format_unicode_finding_message(UnicodeScanResult)` — a new helper in `eval_guard.cpp` exposed only at TU scope (not in the header).

`detail::classify_rejection` (the post-eval classifier in `conversion.cpp` / `run_on_context.cpp`) does **not** need to map the new kind — `UNICODE_SMUGGLE_DETECTED` is only ever set on the pre-eval path, never surfaces through a rejected JS promise, and does not pass through `classify_rejection`. If a future async-op payload legitimately carries the kind (no known caller; flagged in §Out of Scope), the async-bridge `classify_rejection` can add the mapping at that time.

### Pass ordering inside Layer 2

- **Scan runs before the ICD-0.3.1 CPU-timer bracket.** The scan's cost is bounded by source size × 1/(100 MB/s); budgeting it against the CPU limit would double-penalize. The scan-before-bracket ordering is a policy decision and matches §Resolved Open Question #3.
- **Scan runs before the 0.2.2 MAX_CALL_DEPTH enforcement.** The call-depth check lives in the capability dispatcher, not at `JS_Eval`. Since Layer 2's hook is at `JS_Eval`, it is structurally upstream of the dispatcher anyway; the ordering statement is recorded for future readers.
- **Scan runs after `inject_kernel_stdlib` and after `release()` → `clear_global_own_props`.** The stdlib setup does not execute user-submitted source — those are C-function bindings. The scan's input is the host-provided `src` / `source` argument to `JS_Eval`, not the bindings. Ordering-sensitive only in the sense that the BridgeContext must be initialized before any scan (config projection available).

### Relationship to 0.3.5's `eval` / `Function` deletion

Layer 2 and 0.3.5 are complementary, not redundant:

- **0.3.5 deletes `globalThis.eval` and `globalThis.Function`** at `inject_kernel_stdlib`. This closes the in-runtime `eval("code")` path: JS code loaded into the runtime cannot construct and run more JS code.
- **Layer 2 scans at `JS_Eval`.** This closes the pre-runtime source-submission path: the host cannot submit smuggled source bytes into the runtime.

A package could previously ship a clean-looking `server/handlers/shell.js` whose bytes contain a smuggled payload; 0.3.5 wouldn't see it (deletion is a guard against in-runtime rebuilding, not against pre-runtime submission). Layer 2 closes that gap. Together they cover both adjacent surfaces.

---

## Configuration

### `config.yml` schema

Top-level `security` key (new; subsequent security primitives live under the same namespace):

```yaml
security:
  unicode_scanner:
    enabled: true           # default; setting false disables Layer 1 + Layer 2
    threshold: 50           # findings at or above this = rejection
    log_findings: true      # emit audit events on every above-threshold detection
```

Absent section = defaults above. Secure-by-default.

### `Config::parse` additions

Add three scalar fields to `Config`:

```cpp
struct Config {
    // ... existing fields ...
    bool security_unicode_scanner_enabled = true;
    std::size_t security_unicode_scanner_threshold = 50;
    bool security_unicode_scanner_log_findings = true;
};
```

Wire into the existing `Config::parse` yaml reader. Keep the name-spacing style consistent with the kernel convention (underscored flat fields; no nested sub-struct in the in-memory type even though the yaml is nested). This mirrors the pattern established by `plinth::js::runtime_limits_*` fields in 0.3.1.

### `BridgeContext::ConfigProjection` additions

Per DESIGN §5.1, the three scanner fields are copied into `BridgeContext::ConfigProjection`:

```cpp
struct ConfigProjection {
    // ... 8 existing scalars from 0.3.2 ...
    bool security_unicode_scanner_enabled = true;
    std::size_t security_unicode_scanner_threshold = 50;
    bool security_unicode_scanner_log_findings = true;
};
```

The three reserved fields are not consumed by any JS-level binding in 0.4.1 (scanner config is read at the C++ guard layer, not from JS). They are wired for future policy hooks (a capability or binding that wants to introspect scanner policy). The projection-snapshot pattern from 0.3.2 is preserved; the `ConfigProjection` remains a value-copy on `RuntimePool::create_entry`.

---

## Audit Logging

### Event schemas

Three event shapes routed through the existing `plinth::log::audit` (sync writer with the 0.3.4.1 shutdown gate):

**`security.unicode_smuggle_detected`** — one event per above-threshold detection, subject to rate limit:

```json
{
  "event": "security.unicode_smuggle_detected",
  "layer": "install" | "eval",
  "source_path": "server/handlers/shell.js" | "<eval>" | "<pool>" | "<async>",
  "total_count": 127,
  "threshold": 50,
  "first_findings": [
    {"byte_offset": 42, "codepoint": 65024, "range_name": "variation-selector"}
  ],
  "decode_error": null | "invalid continuation byte at offset 42"
}
```

**`security.unicode_smuggle_rate_limited`** — one event when the per-`(layer, source_path)` token bucket overflows; emitted at most once per bucket-interval (1 Hz):

```json
{
  "event": "security.unicode_smuggle_rate_limited",
  "layer": "install" | "eval",
  "source_path": "<path>",
  "suppressed_count": 42
}
```

**`security.unicode_scanner_disabled`** — one-shot event at kernel startup if `security.unicode_scanner.enabled: false`:

```json
{
  "event": "security.unicode_scanner_disabled",
  "config_origin": "config.yml" | "default"
}
```

### Rate-limit policy

Per §Resolved Open Question #4: a 1 Hz token bucket keyed on `(layer, source_path)`. Implementation note: a small in-memory LRU (64 entries is sufficient for any realistic workload) keyed on the tuple, storing the last-emit timestamp. Overflow emits one `security.unicode_smuggle_rate_limited` with the `suppressed_count` accumulated since the last emit. Bucket lives in a file-static in `eval_guard.cpp` for Layer 2 and in `validator.cpp` for Layer 1; the two rate limits are independent (Layer 1 runs in the CLI process, Layer 2 runs in the kernel process — they don't share state by construction).

### `log_findings: false` semantics

Gate still rejects; audit writes are suppressed. Layer 1 still emits the `ValidationMessage`; Layer 2 still returns `std::unexpected(EvalError{...})`. Only the audit trail goes silent. The `security.unicode_scanner_disabled` event does NOT fire based on `log_findings` — that event is specifically about `enabled: false`. A `log_findings: false` + `enabled: true` configuration is a legitimate "mute audit volume, keep the gate" posture.

### spdlog (non-audit)

Kernel-side `spdlog::warn("unicode-smuggle detected: layer={} path={} count={} threshold={}", ...)` on every above-threshold event, independent of `log_findings` (spdlog is developer-diagnostic; audit is compliance). No new spdlog surface required.

---

## Test Battery

All cases under `tests/kernel/security/unicode_scanner_test.cpp` (unit), `tests/kernel/js/eval_guard_test.cpp` (Layer 2 integration), and existing `tests/kernel/packages/validator_test.cpp` (Layer 1 integration). Case IDs prefixed `G.` (GlassWorm), numbered contiguously.

| # | Test | File | Setup | Expected |
|---|------|------|-------|----------|
| G.01 | Scanner — clean ASCII | `unicode_scanner_test.cpp` | 1 KiB of ASCII with no non-ASCII bytes | `total_count == 0`, `exceeds_threshold == false`, `first_findings.empty()`, `decode_error == nullopt` |
| G.02 | Scanner — legitimate emoji (1 variation selector) | `unicode_scanner_test.cpp` | `"👍\uFE0F"` (thumbs-up + VS-16) | `total_count == 1`, `exceeds_threshold == false`, `first_findings[0].range_name == "variation-selector"` |
| G.03 | Scanner — threshold boundary (49 variation selectors) | `unicode_scanner_test.cpp` | 49 × U+FE0F | `total_count == 49`, `exceeds_threshold == false` |
| G.04 | Scanner — threshold boundary (50 variation selectors) | `unicode_scanner_test.cpp` | 50 × U+FE0F | `total_count == 50`, `exceeds_threshold == true`; `first_findings.size() == 5` |
| G.05 | Scanner — bidi override burst | `unicode_scanner_test.cpp` | 55 × U+202E (RLO) | `exceeds_threshold == true`; `first_findings[0].range_name == "bidi-override"` |
| G.06 | Scanner — zero-width + BOM + tag burst | `unicode_scanner_test.cpp` | 20 × U+200B + 20 × U+FEFF + 20 × U+E0020 | `total_count == 60`, `exceeds_threshold == true`; `first_findings` sampled across all three ranges |
| G.07 | Scanner — malformed UTF-8 | `unicode_scanner_test.cpp` | Byte sequence `0xC0 0x80` (overlong) mid-source | `decode_error.has_value()`, `exceeds_threshold == true` |
| G.08 | Scanner — first_findings bound (100 entries, record_first_n=5) | `unicode_scanner_test.cpp` | 100 × U+FE0F | `total_count == 100`, `first_findings.size() == 5` |
| G.09 | Layer 1 — smuggle fixture (variation selectors) | `validator_test.cpp` | `tests/fixtures/packages/unicode-smuggle-variation-selectors/` | Report contains `unicode-smuggle` with `Severity::ERROR`; disposition = 1 |
| G.10 | Layer 1 — smuggle fixture (bidi override) | `validator_test.cpp` | `tests/fixtures/packages/unicode-smuggle-bidi-override/` | Report contains `unicode-smuggle` with `Severity::ERROR`; disposition = 1 |
| G.11 | Layer 1 — malformed UTF-8 fixture | `validator_test.cpp` | `tests/fixtures/packages/unicode-smuggle-malformed-utf8/` | Report contains `unicode-smuggle` citing `decode_error`; disposition = 1 |
| G.12 | Layer 1 — legitimate emoji fixture baseline | `validator_test.cpp` | `tests/fixtures/packages/unicode-legitimate-emoji/` | No `unicode-smuggle` finding; disposition = 0 |
| G.13 | Layer 1 — `valid-full/` regression | `validator_test.cpp` | Existing 0.4.0 `valid-full/` fixture | No `unicode-smuggle` finding (regression guard) |
| G.14 | Layer 2 — one-shot eval rejects smuggled source | `eval_guard_test.cpp` | `plinth::js::eval()` called with 60 × U+FE0F inlined in valid JS | `std::unexpected(EvalError{kind: UNICODE_SMUGGLE_DETECTED})`; `EvalError::message` contains total count + first-five offsets |
| G.15 | Layer 2 — pooled eval rejects smuggled source | `eval_guard_test.cpp` | `RuntimePool::eval_on_context(bc, smuggled_src)` | Same as G.14 |
| G.16 | Layer 2 — async eval rejects smuggled source | `eval_guard_test.cpp` | `eval_on_context_async(bc, smuggled_src).get()` via Drogon coroutine test harness | Same as G.14; no hang, no ASAN finding |
| G.17 | Layer 2 — clean source still evaluates | `eval_guard_test.cpp` | `eval("1 + 1")` with zero findings | Returns `Json::Value(2)` as before; scanner does not block legitimate code |
| G.18 | Benchmark — 100 MB/s throughput | `unicode_scanner_benchmark.cpp` | 1 MiB clean-ASCII payload, 100 iterations | Google Benchmark reports ≥ 100 MB/s on CI builder image. **Gated behind `PLINTH_BENCHMARKS=ON` — informational in CI, not a ctest-blocking case.** |

**Case count:** 18 cases. G.01–G.08 are scanner-unit (no kernel deps); G.09–G.13 are Layer 1 integration; G.14–G.17 are Layer 2 integration; G.18 is a benchmark-harness acceptance. The ≥10 floor from DESIGN §6.1 is cleared.

### Fixture construction

Four new directories under `tests/fixtures/packages/`:

- **`unicode-smuggle-variation-selectors/`** — minimal valid package structure (manifest.json + capabilities.json + `server/main.js` + `server/handlers/*.js`); the `server/handlers/shell.js` file contains 100 × U+FE0F embedded in a valid-JS comment (comment so the file parses if accidentally reached). All other files are clean.
- **`unicode-smuggle-bidi-override/`** — same shell as above but with 55 × U+202E in a string literal inside `server/handlers/shell.js`.
- **`unicode-smuggle-malformed-utf8/`** — overlong-UTF-8 sequence in `server/handlers/shell.js`. The fixture is committed as a raw byte sequence; a pre-commit check (existing `prepare_dynamic_fixtures()` safety-net from 0.4.0) verifies the file remains byte-exact on checkout.
- **`unicode-legitimate-emoji/`** — 5 different emoji each followed by exactly one variation selector inside `server/handlers/shell.js`. Total count 5, well under threshold.

All four fixtures reuse the 0.4.0 `valid-minimal/` skeleton for manifest / capabilities / `server/main.js`; the divergence is only in `server/handlers/shell.js`.

### Sanitizer + tidy

- ASan + UBSan: G.07 (malformed UTF-8) and G.11 (malformed fixture) are the high-signal cases. Must report zero sanitizer findings.
- `run-clang-tidy-20` on the new TUs (`unicode_scanner.{hpp,cpp}`, `eval_guard.{hpp,cpp}`): zero findings. `tests/kernel/security/.clang-tidy` inherits from the tests-kernel config (matches the 0.3.3.2 convention).

### CI wiring

No new CI job. The existing `build-and-test` step's `KERNEL_SOURCES` glob (widened in 0.3.3.2 to cover `tests/kernel/**`) captures the new test files without CMake edits. The benchmark step (PLINTH_BENCHMARKS informational block in `ci.yml`) picks up `benchmarks/unicode_scanner_benchmark.cpp` by its glob.

PG-backed tests are not required for any G-case; the scanner is pure, Layer 2 integration uses the existing `BridgeContext` harness (no DB), and Layer 1 is filesystem-only.

---

## Security Constraints (Non-Negotiable)

1. **Scanner is pure.** No kernel state read or written inside `scan_for_invisible_unicode`. Pure function of `(source, cfg)`. This is load-bearing for Layer 1's CLI use case — the validator runs in a CLI process with no kernel runtime, no DB, no Drogon app.
2. **No allocations on the clean path.** The zero-finding common case must not heap-allocate (beyond the one reserved `first_findings` vector). Measured by the benchmark harness; regression is a blocker.
3. **Scanner failure = gate failure.** Malformed UTF-8 (`decode_error.has_value()`) rejects, not skips. A scanner that could be defeated by splicing invalid bytes into a smuggled payload is no gate at all.
4. **No per-package or per-request override.** Confirmed non-negotiable in DESIGN §7 — overrides are attack vectors. The only knob is the global `security.unicode_scanner.enabled` flag, which takes an audit event on use (§Audit).
5. **Pre-eval scan runs before any in-runtime work.** If `pre_eval_scan` returns `std::unexpected`, the integrating call site MUST NOT call `JS_Eval`. A future refactor that moves the scan after `JS_Eval` (even for measurement) would break the security model. Tests G.14–G.16 pin this by asserting that no side-effect from `JS_Eval` is observable on the rejection path.
6. **Audit event emission is best-effort.** If the audit writer is not yet ready (`plinth::log::audit` gated on `g_audit_ready` per 0.2.4), the gate still rejects — the security decision never depends on the audit path being live. This is the same invariant 0.2.4 established for RBAC denials.
7. **`UNICODE_SMUGGLE_DETECTED` is a pre-eval-only kind.** `detail::classify_rejection` does not map it. A promise rejection carrying `code = "async.unicode_smuggle"` is NOT a supported contract in 0.4.1; if a future caller needs it, add through a new ICD.

---

## Resolved Open Questions

DESIGN §8's six Open Questions are ratified here with the proposed defaults. Ratification is via this ICD's PR merge; no further architect sign-off is required for these six. (Matches the 0.3.5 "Open Questions tacit ratification" precedent.)

1. **Threshold default.** `threshold = 50`. Tune on first false-positive evidence; no pre-ship calibration against real extensions. The `security.unicode_scanner.threshold` config field exposes the knob; raising it globally is an operator decision.
2. **BOM (U+FEFF) policy.** Always counted in `total_count`. A legitimate leading-BOM file contributes 1 to the count, well under the default threshold. No positional exception — the scan is bytes-forward with no per-offset semantic. Implementation simpler; no BOM-stripping pass needed before scan.
3. **Layer 2 scan position.** Before the call-depth check, before the CPU-timer bracket, after `inject_kernel_stdlib`. See §Layer 2 / Pass ordering.
4. **Audit rate limit.** 1 Hz token bucket per `(layer, source_path)`; overflow emits a single `security.unicode_smuggle_rate_limited` with `suppressed_count`. See §Audit / Rate-limit policy.
5. **Unicode normalization.** None. Scan raw UTF-8 bytes as written. Normalization (NFC / NFD / NFKC / NFKD) could fold smuggled sequences into or out of the scan ranges unpredictably.
6. **Disabled-state audit.** Yes. `security.unicode_scanner_disabled` fires once at kernel startup if `security.unicode_scanner.enabled: false`. Low implementation cost; high value as a compliance breadcrumb.

---

## Milestone Criteria

All of the following MUST be true for 0.4.1 to close:

1. **All 18 G-cases pass** under Catch2 default and sanitizer builds (G.18 is gated behind `PLINTH_BENCHMARKS=ON` and informational — its failure does not block ctest but its throughput number is cited in the PR).
2. **Benchmark ≥ 100 MB/s** single-thread on the v0.4.0 CI builder image (Ubuntu 25.10, clang-20), for the clean-ASCII baseline. Regression > 5% vs pre-0.4.1 reference on the same harness is a blocker. Recorded in the PR body.
3. **No existing test regressions.** `ctest` suite-wide pass count increases by exactly the new cases (or by the new cases plus documented pre-0.4.1 flakes). 0.4.0's 38-case package suite remains green; 0.3.x's JS suite remains green.
4. **Clear error messages.** Both Layer 1 and Layer 2 reject messages cite the `unicode-smuggle` rule / `UNICODE_SMUGGLE_DETECTED` kind by name, include total count + threshold, and list the first-five `UnicodeFinding` offsets. Verified by test-message asserts (G.09–G.11, G.14–G.16).
5. **No GlassWorm payload reaches `JS_Eval` through any validated path.** Adversarial case: a package with 100 × U+FE0F in `server/handlers/shell.js` is rejected at Layer 1 (G.09), at Layer 2 if a test synthesizes the source directly (G.14–G.16). No code path allows the smuggled source to pass both gates.
6. **Legitimate extensions continue working.** 0.4.0's `valid-full/` fixture passes validation with zero Layer 1 findings (G.13). The `unicode-legitimate-emoji/` fixture (5 emoji × 1 VS each) passes (G.12).
7. **`run-clang-tidy-20` zero findings** on `src/kernel/security/**`, `src/kernel/js/eval_guard.{hpp,cpp}`, and the modified portions of `src/kernel/packages/validator.cpp`, `src/kernel/js/eval.cpp`, `src/kernel/js/runtime_pool.cpp`, `src/kernel/js/run_on_context.cpp`.
8. **CHANGELOG 0.4.1 entry** enumerates: the scanner TU, the guard TU, the validator pass, the three `JS_Eval` integrations, the new `EvalErrorKind` variant, the four fixtures, the 18 G-cases, the benchmark number, the six Resolved Open Questions (for audit trail), and any accepted deviations. `v0.4.1` tag applied (3-part milestone per `feedback_tagging_rule.md`).
9. **Architecture doc touch:** `architecture/05-extensions.md §3.1` gains a one-line pointer to `DESIGN-glassworm-defense-v0x.md` / this ICD in the Runtime Limits section. Deferred to 0.4.1 code session per RE-EVAL-0.4.x.md §10 suggestion.

---

## Entry / Exit

**Entry:**

- 0.4.0 shipped (validator + `ValidationReport` surface exists on main) — `git log` confirms PR #40 merged 2026-04-19.
- 0.3.5 shipped (QuickJS hardening surface with `eval` / `Function` deletion at `inject_kernel_stdlib`) — PR #38 merged 2026-04-19.
- 0.4.0.1 shipped (deterministic teardown; `drain_pending_jobs` landed, hence the runtime_pool.cpp line shift noted above) — PR #42 merged 2026-04-19.
- RE-EVAL-0.4.x + DESIGN-glassworm-defense-v0x + this ICD merged to main.

**Exit:**

- All eight Milestone Criteria satisfied.
- `feat/0.4.1-glassworm-defense` branch merged to main as PR; squash commit tagged `v0.4.1`.
- ROADMAP 0.4.1 line removed per preamble rule (completed milestones are trimmed; history lives in CHANGELOG).

---

## Appendix — Integration Checklist for the Code Session

For the implementing session's convenience, the concrete files-touched set:

**New TUs:**
- `src/kernel/security/unicode_scanner.hpp` — declarations per §Scanner Primitive.
- `src/kernel/security/unicode_scanner.cpp` — single-pass UTF-8 decoder.
- `src/kernel/js/eval_guard.hpp` — `pre_eval_scan` declaration.
- `src/kernel/js/eval_guard.cpp` — `pre_eval_scan` + `format_unicode_finding_message` + file-static rate-limit LRU + audit-emit helper.

**Modified TUs:**
- `src/kernel/packages/validator.cpp` — insert `run_unicode_scan_pass()` call in `validate()`; add the `run_unicode_scan_pass` + file-scanning helpers in the anonymous namespace.
- `src/kernel/js/eval.cpp` — 2-line insertion above `JS_Eval` at line 277.
- `src/kernel/js/runtime_pool.cpp` — 2-line insertion above `JS_Eval` at line 756.
- `src/kernel/js/run_on_context.cpp` — 2-line insertion above `JS_Eval` at line 816.
- `src/kernel/js/eval.hpp` — add `EvalErrorKind::UNICODE_SMUGGLE_DETECTED`.
- `src/kernel/js/bridge_context.hpp` — extend `ConfigProjection` with three scanner fields.
- `src/kernel/js/runtime_pool.cpp` — populate the three `ConfigProjection` fields from `Config` in `create_entry`.
- `src/kernel/config.{hpp,cpp}` (or equivalent) — add three `security_unicode_scanner_*` fields and YAML parsing.

**New test files:**
- `tests/kernel/security/unicode_scanner_test.cpp` — G.01–G.08.
- `tests/kernel/js/eval_guard_test.cpp` — G.14–G.17.

**Extended existing test file:**
- `tests/kernel/packages/validator_test.cpp` — G.09–G.13.

**New benchmark:**
- `benchmarks/unicode_scanner_benchmark.cpp` — G.18.

**New fixtures:**
- `tests/fixtures/packages/unicode-smuggle-variation-selectors/`
- `tests/fixtures/packages/unicode-smuggle-bidi-override/`
- `tests/fixtures/packages/unicode-smuggle-malformed-utf8/`
- `tests/fixtures/packages/unicode-legitimate-emoji/`

**Config file:**
- `config.yml.example` — add the `security.unicode_scanner` section with documented defaults.

No CMake edits (KERNEL_SOURCES glob captures the new TUs and test files; benchmarks glob captures the new harness). No CI-YAML edits. No new clang-tidy configuration.
