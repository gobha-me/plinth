# ICD-0.3.0-quickjs-vendoring

**Traces to:** architecture/05-extensions.md §3 (QuickJS Runtime and Extension Supervision), DESIGN-quickjs-bridge.md §2 (The Actors: QuickJS Runtime), DESIGN-quickjs-bridge.md §9.0 (Implementation Sequence: Vendored + Basic Eval)
**Depends on:** none — first ICD of the 0.3.x arc
**Milestone:** 0.3.0 — QuickJS vendored, basic eval from C++
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** DESIGN-quickjs-bridge.md §§1, 3.1, 10 (later milestones build on this foundation)

---

## Overview

This ICD defines the vendoring of QuickJS into the Plinth source tree and the minimal host-side `eval()` surface that proves QuickJS compiles, links, and runs correctly within the Plinth process. Its scope is deliberately narrow — 0.3.0 is a compile-and-link milestone, not a feature milestone.

Out of scope for 0.3.0 (covered by later ICDs):
- Runtime pool (ICD-0.3.1)
- Resource limits beyond a single hard-coded memory ceiling proving the mechanism works (ICD-0.3.1)
- Kernel API injection (ICD-0.3.2)
- Async bridge / promise plumbing / `drogon::Task<>` interop (future ICD against `DESIGN-quickjs-bridge.md §3` for milestone 0.3.3)

---

## Vendoring Contract

### Source Acquisition

QuickJS is vendored via **CMake `FetchContent`** against a pinned upstream tag. Rationale: the same mechanism is already used for Google Benchmark (`benchmarks/CMakeLists.txt`, 0.2.6.2) and Drogon, keeping the third-party acquisition model consistent. No git submodule; no hand-copied source tree.

**Pinned tag selection:** pick the latest stable QuickJS release tag at implementation time (record the exact tag + commit SHA in `CMakeLists.txt` and the CHANGELOG entry). If no stable release is newer than the last Fabrice Bellard drop, use the latest `bellard/quickjs` tag; if the Fabrice Bellard repo is stale at session time, fall back to the `quickjs-ng/quickjs-ng` fork — this choice is a code-session decision, not a structural one, and must be captured in the CHANGELOG.

### Build Target

- **Static library target:** `plinth_quickjs` (no namespace alias in 0.3.0 — add `plinth::quickjs` alias only if a consumer ever needs it).
- **C standard:** C11 (QuickJS requirement).
- **Build flags (required):** position-independent code (`-fPIC`); warnings suppressed for vendored sources only (`-w` or file-scoped suppression) to keep `-Wall -Wextra -Werror` on Plinth's own translation units.
- **Build flags (opt-in):** LTO follows the parent Plinth CMake `-DPLINTH_LTO` flag if present; NaN-boxing is the QuickJS default and is not disabled.
- **Opt-out flag:** none in 0.3.0 — QuickJS is always built. A `-DPLINTH_QUICKJS=OFF` flag may be added in 0.3.1+ if CI times regress.

### Include Path

Plinth's own sources include QuickJS headers as:

```cpp
#include <quickjs.h>
```

The header is exposed via a `target_include_directories(plinth_quickjs PUBLIC ...)` in the FetchContent integration. No Plinth translation unit may `#include "third_party/..."` directly.

### Platform Support

- **Linux x86_64, gcc 13 and clang 18:** required. These match the CI builder image (see `project_plinth_state` v0.1.5.1 / v0.2.1.1a).
- **Other platforms:** deferred. If a non-Linux build is attempted in 0.3.0, it is expected to fail fast at CMake configure time, not produce mysterious runtime errors.

---

## Eval API (C++ Side)

### Function Signature

```cpp
// src/kernel/js/eval.hpp
namespace plinth::js {

enum class EvalErrorKind {
    SYNTAX_ERROR,     // QuickJS parse failure
    RUNTIME_ERROR,    // Uncaught JS exception during execution
    MEMORY_LIMIT,     // Allocation refused by JS_SetMemoryLimit
    INTERNAL          // Host-side failure (runtime creation, etc.)
};

struct EvalError {
    EvalErrorKind kind;
    std::string message;  // Human-readable — includes QuickJS message + stack if available
    int line = 0;         // 1-based; 0 if unavailable
    int column = 0;       // 1-based; 0 if unavailable
};

// One-shot eval: creates a fresh JSRuntime + JSContext, evaluates `src`,
// converts the result to Json::Value, destroys the runtime.
//
// No runtime reuse, no module loader, no kernel APIs.
// Returns the evaluated expression's value on success.
auto eval(std::string_view src) -> std::expected<Json::Value, EvalError>;

}  // namespace plinth::js
```

Notes:
- `std::expected` is already in use elsewhere in the kernel (C++23 via clang-18 / gcc-13); if it is unavailable, fall back to the project's existing `Result<T, E>` pattern as used in `src/kernel/capabilities/resolution.hpp` — this is a code-session call, not a structural decision.
- `Json::Value` is the same JsonCpp type already used by the capability resolver; result conversion reuses whatever JS↔JSON helper lands with this ICD (a single private `js_to_json` in `eval.cpp` is sufficient — no public JS↔JSON utility surface in 0.3.0).

### Memory Limit (Hard-Coded)

Every runtime created by `eval()` MUST have `JS_SetMemoryLimit` called with a non-zero ceiling before any JS code runs. In 0.3.0 the ceiling is **hard-coded to 16 MiB** — this is deliberately rigid so the memory-limit test path is exercised from day one. Per-runtime configurable budgets land in ICD-0.3.1.

### Supported JS Subset

- ECMAScript features: whatever the pinned QuickJS tag supports. No polyfills, no transpilation.
- Module system: **not enabled**. `eval()` takes a single source string; `import` / `export` produce `SYNTAX_ERROR`.
- `eval()` inside JS (the ECMA builtin): left at QuickJS defaults for 0.3.0. Disabling `eval` within JS is the concern of ICD-0.3.1 (runtime lifecycle hardening).

---

## Error Model

| Condition | `EvalErrorKind` | `message` content |
|---|---|---|
| Parse failure | `SYNTAX_ERROR` | QuickJS parser diagnostic verbatim |
| Throw at runtime (e.g. `throw new Error("x")`) | `RUNTIME_ERROR` | `<error.name>: <error.message>\n<stack>` |
| Allocation refused by memory limit | `MEMORY_LIMIT` | `"out of memory: JS heap exceeded <N> bytes"` |
| Runtime creation failure, or any unexpected C++-side fault | `INTERNAL` | Implementation-defined diagnostic |

`line` and `column` are populated for `SYNTAX_ERROR` and `RUNTIME_ERROR` whenever QuickJS supplies them; otherwise both are `0`.

No capture of `console.log` output in 0.3.0 — `console` is not defined. Any JS that references `console` produces `RUNTIME_ERROR`.

---

## Performance Targets

- **Cold eval of `"1 + 1"` (runtime create → eval → runtime destroy):** under **50 ms** on the CI builder image.
- **Leak behavior:** 1 000 sequential `eval("1 + 1")` calls under ASAN must report zero leaks.

These are sanity checks, not production targets. Tight per-operation budgets come in ICD-0.3.1 once the runtime pool exists and eliminates the per-call create/destroy overhead.

---

## Security Constraints (Non-Negotiable)

1. `plinth::js::eval()` is **host-side only**. No JS execution path in 0.3.0 is reachable from HTTP, WebSocket, PAT, or any capability dispatch. The function is callable only from Plinth C++ code and from Catch2 test binaries.
2. Every `JSRuntime` created by this module MUST have `JS_SetMemoryLimit` invoked with a non-zero value before any `JS_Eval` call. A runtime constructed without a memory limit is a programming error.
3. The hard-coded 16 MiB ceiling MUST NOT be circumvented via environment variables, config files, or runtime flags in 0.3.0. The only override path is the code change that lands in ICD-0.3.1.

---

## What Must Not Be Decided Yet

The following belong to later ICDs and MUST NOT be pre-empted by 0.3.0 code:

- **Runtime pool design** (acquire/release/reset semantics, sizing, on-demand growth) — ICD-0.3.1.
- **Configurable resource limits** (per-extension memory / CPU / wall-clock / stack budgets) — ICD-0.3.1.
- **Kernel API injection** (`log.*`, `config.*`, `crypto.*`) — ICD-0.3.2.
- **Async bridge** (`drogon::Task<>` interop, promise↔coroutine plumbing, `JS_ExecutePendingJob` loop, cancellation cascade) — 0.3.3 ICD, against `DESIGN-quickjs-bridge.md §§3, 6`.
- **Return-value shape of `plinth.call()`** — stays opaque at the bridge (see `DISCUSSION-streaming-and-media.md §0`); 0.3.0 does not introduce any JS-visible plinth-namespace object.

---

## Milestone Criteria

All four test cases below MUST pass under Catch2 on the CI builder image before 0.3.0 ships. They mirror the bullets in `DESIGN-quickjs-bridge.md §9.0`.

### Tests

1. **Simple eval:** `eval("1 + 1")` returns `Json::Value` holding integer `2`.
2. **Syntax error surfaces cleanly:** `eval("function (")` returns `EvalError{ kind = SYNTAX_ERROR }` with a non-empty `message`; no crash, no leak.
3. **Memory-limit exhaustion:** `eval(R"js(let a=[]; while(true) a.push(new Array(100000).fill(0));)js")` returns `EvalError{ kind = MEMORY_LIMIT }` within bounded time (the interrupt handler is not installed yet in 0.3.0, so the escape hatch is the allocation-refused path, not a CPU-time cut-off).
4. **Create / destroy leak check:** a loop of 1 000 `eval("1 + 1")` calls under `-DPLINTH_SANITIZERS=ON` (ASAN + UBSan) reports zero leaks and zero UB.

### CI Wiring

- `eval.cpp` / `eval.hpp` added under `src/kernel/js/`.
- Test file at `tests/kernel/js/eval_test.cpp` registered in the existing Catch2 test executable.
- FetchContent declaration in the top-level `CMakeLists.txt` or a dedicated `cmake/quickjs.cmake` file.
- No new CI job — the existing `build-and-test` job exercises all four cases.
