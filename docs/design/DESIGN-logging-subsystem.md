# DESIGN-logging-subsystem

**Status:** Approved v1  
**Scale:** 1 (Foundational technology decision)  
**Traces to:** architecture/04-services-ha.md §1 (Audit Logging), architecture/02-capabilities.md §2 (Kernel Standard Library), architecture/05-extensions.md §3 (QuickJS Runtime)  
**Milestone:** 0.1.7 (Audit log) and all subsequent work  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Decision Date:** 2026-04-15  
**Author:** the maintainer (Architect) + Grok 4.20 Multi-Agent (Architecture Session)

---

## Decision

**Use spdlog as the logging subsystem.**

spdlog will be the single source of truth for all kernel, extension, and audit-adjacent logging. It replaces Drogon’s built-in logger entirely.

### Why spdlog (Rejected Alternatives)

| Option              | Rejected Because |
|---------------------|------------------|
| Drogon built-in     | Blocking, limited sinks, no async mode, poor rotation |
| Boost.Log           | Heavy compile times, complex API, not header-only |
| Custom C++ logger   | Reinventing the wheel; we gain nothing |
| **spdlog**          | Header-only, extremely fast, async mode, multiple sinks, pattern formatting, widely used in C++ projects, familiar to architect |

**Version:** spdlog v1.15.0 (or latest compatible with C++20 at implementation time). Vendored via CMake FetchContent to avoid system dependency.

---

## Logging API (C++ Kernel)

All kernel code uses the following macros (defined in `src/kernel/logging.hpp`):

```cpp
log::trace("message {}", arg);
log::debug("message {}", arg);
log::info("message {}", arg);
log::warn("message {}", arg);
log::error("message {}", arg);
log::critical("message {}", arg);

log::audit("action", detail_json);  // Special path — writes to plinth.audit_log
```

- Format uses spdlog’s `{}` syntax (fmtlib).
- All logs are asynchronous by default (queued to a background thread).
- `log::audit()` bypasses the normal sink and writes directly to the audit table (see `architecture/04-services-ha.md §1`). It is **not** subject to normal log level filtering.

**Default level:** `info` in production, `debug` in `dev_mode`.

---

## Logging API (QuickJS / Extensions)

Injected into every JS runtime (see DESIGN-quickjs-bridge.md and `architecture/02-capabilities.md §2`):

```javascript
log.trace("message", data);   // data is optional object
log.debug("message", data);
log.info("message", data);
log.warn("message", data);
log.error("message", data);
log.critical("message", data);

audit.log("action_name", { /* structured detail */ });
```

These map directly to the C++ layer. Objects passed to `log.*` are JSON-serialized. `audit.log()` is the JS equivalent of the C++ audit path.

---

## Configuration & Sinks

**Sinks (in order):**
1. **Console sink** (colored output in dev_mode, plain in production)
2. **Rotating file sink** (`logs/plinth.log`, max 10 MB per file, 5 files retained, compressed on rotation)
3. **Audit sink** (for `log::audit` / `audit.log()` — writes to `plinth.audit_log` table, not filesystem)

**Async settings:**
- Queue size: 8192 messages
- Flush interval: 1 second (or on critical errors)
- Overflow policy: block (we prefer backpressure over data loss in early versions)

**Configuration keys** (in kernel config.json):
```json
{
  "logging": {
    "level": "info",
    "async": true,
    "console": true,
    "file": { "enabled": true, "path": "logs/plinth.log" }
  }
}
```

---

## Performance & Threading Rules

- Hot-path code (capability dispatch, DB layer, realtime coalescer) **must prefer `log::trace`/`debug`** and should be compiled out in release builds when possible.
- All logging calls from JS are routed through the bridge coroutine (non-blocking to the extent possible).
- spdlog logger is created once at kernel startup and shared (thread-safe by design).
- Never use `std::cout`, `printf`, or Drogon’s logger directly.

---

## What Must Not Be Decided Yet

- Structured logging format beyond basic JSON serialization of extra data (deferred until metrics/observability extension).
- Log aggregation, centralization, or shipping to external systems (e.g., Loki, ELK) — this belongs in an extension or sidecar, not the kernel.
- Runtime log level changing per-extension (beyond global + dev_mode).
- Integration with future observability/metrics pipeline (0.7+).
- Any change to the JS `log.*` or `audit.log` surface without an architecture session.

---

## Milestone Criteria (0.1.7)

**Entry:** 0.1.6 (WebSocket) complete.  
**Exit:**
- spdlog integrated and initialized in `main.cpp`.
- All `log.*` and `audit.log` APIs work from both C++ and QuickJS (after 0.3).
- Rotating file + console sinks functional.
- Audit path writes to `plinth.audit_log` table.
- Catch2 tests cover all log levels and audit path.
- Scaffold updated, SESSION-GUIDE.md references this document.
- Human approval of implementation plan before any logging code is written.

---

## Open Questions

1. Exact rotation parameters (10 MB / 5 files) — confirm via load testing in 0.7.
2. Whether to add a syslog sink for production deployments (post-1.0).
3. JSON vs key-value formatting for file logs (decide before 0.10 security audit).

---

**This document is the permanent authority on all logging decisions.** Any code session touching logging, audit, or the QuickJS standard library **must** read this document first. Changes to this contract require a new architecture session and updated design document.