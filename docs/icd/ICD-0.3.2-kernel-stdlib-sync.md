# ICD-0.3.2-kernel-stdlib-sync

**Traces to:** architecture/05-extensions.md §3 (QuickJS Runtime and Extension Supervision), DESIGN-quickjs-bridge.md §8 (Integration with Kernel Systems — sync subsets only), DESIGN-quickjs-bridge.md §9.2 (Implementation Sequence: Kernel Standard Library — Synchronous)
**Depends on:** ICD-0.3.1-runtime-lifecycle (BridgeContext, RuntimePool, EvalError enum), ICD-0.3.0-quickjs-vendoring (static library + `eval` core)
**Milestone:** 0.3.2 — Kernel standard library injection (db, log, audit, config — **sync subset only in this ICD**)
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** DESIGN-logging-subsystem.md, `src/kernel/logging.hpp` (the `plinth::log::*` functions this ICD wraps), `src/kernel/auth/crypto.hpp` (existing argon2id surface — referenced but not re-exposed by `crypto.*`), DISCUSSION-streaming-and-media.md §0 (keeps `plinth.call()` return shape opaque; this ICD does NOT introduce a JS-visible `plinth.call`)

---

## Overview

This ICD defines the **synchronous** subset of kernel APIs injected into every QuickJS runtime created by `RuntimePool` (ICD-0.3.1): `log.*`, `config.get()`, and `crypto.*`. These are pure C→JS-function bindings with no promises and no `await` — they execute entirely on the single QuickJS thread and return directly.

Synchronous surface is deliberately limited in 0.3.2. The async surface (`db.*`, `audit.*`, `cap.*`, `pubsub.*`, `storage.*`, `http.*`) requires the promise↔coroutine bridge and lands in 0.3.3 + 0.3.4 against `DESIGN-quickjs-bridge.md §§3, 5, 8`. The roadmap entry for 0.3.2 names "db, log, audit, config" — this ICD implements **only log, config, and crypto**, because `db.*` and `audit.*` both require async bridging per `DESIGN-quickjs-bridge.md §§3.3, 8.1`. This ICD documents that scoping decision explicitly and defers `db.*` / `audit.*` to 0.3.3 with a matching ROADMAP note in the CHANGELOG entry.

---

## Injected Surface

Every runtime created by `RuntimePool` has the following globals available from JS before the first host-called function executes. All are plain JS functions — not objects on `globalThis.plinth` — matching the flat `log.*` / `config.*` / `crypto.*` shape in `DESIGN-quickjs-bridge.md §9.2`.

### `log.*`

```
log.debug(msg: string, ctx?: object) -> undefined
log.info (msg: string, ctx?: object) -> undefined
log.warn (msg: string, ctx?: object) -> undefined
log.error(msg: string, ctx?: object) -> undefined
```

Behavior:
- Forwards to `plinth::log::debug/info/warn/error` (see `src/kernel/logging.hpp`) verbatim.
- `ctx` is serialized to a single-line JSON string and appended as `" ctx={...}"` — spdlog does not understand structured args out of the box in the Plinth code, so a string append is the simplest fit.
- `ctx` omitted → no suffix.
- Return value: `undefined`. Never throws.

### `config.get(key)`

```
config.get(key: string) -> any | null
```

Behavior:
- Looks up `key` in a **string-keyed projection** of the loaded `plinth::Config` struct. The projection is the explicit public surface — NOT a reflection of every struct field.
- Returns `null` if the key is not in the projection.
- Throws `TypeError` if `key` is not a string.
- Never returns or reveals secret-flagged values (see Security Constraints).

The initial projection is:

| Key | Source field | Type |
|---|---|---|
| `"dev_mode"` | `Config::dev_mode` | boolean |
| `"node_id"` | `Config::node_id` | string |
| `"listen_host"` | `Config::listen_host` | string |
| `"listen_port"` | `Config::listen_port` | number |
| `"registration_enabled"` | `Config::registration_enabled` | boolean |
| `"ws.auth_timeout_s"` | `Config::ws_auth_timeout_s` | number |
| `"ws.heartbeat_interval_s"` | `Config::ws_heartbeat_interval_s` | number |
| `"ws.heartbeat_timeout_s"` | `Config::ws_heartbeat_timeout_s` | number |

**Explicitly excluded from the projection:** every `Config::Database` field (`db.host`, `db.port`, `db.user`, `db.password`, `db.database`, `db.pool_size`), and `migrations_dir`. These are secret-flagged or operator-only and MUST NOT be readable from JS.

The projection is hard-coded in `config_bindings.cpp` as a static table — no reflection, no runtime opt-out for secrets. Adding a key is a PR.

### `crypto.*`

```
crypto.hash(alg: "sha256" | "sha512", data: string | Uint8Array) -> string  // lowercase hex
crypto.randomBytes(n: int) -> Uint8Array
crypto.timingSafeEqual(a: Uint8Array, b: Uint8Array) -> boolean
```

Behavior:
- `hash`: SHA-256 or SHA-512 via OpenSSL (already linked via argon2 / Drogon). Strings are hashed as their UTF-8 byte sequence. Output is lowercase hex, no `0x` prefix.
- `randomBytes`: uses `RAND_bytes()` from OpenSSL. `n` bounded `1 ≤ n ≤ 4096` — outside that range throws `RangeError`. On OpenSSL failure (extraordinarily rare), throws `Error("crypto.randomBytes: RNG failure")`.
- `timingSafeEqual`: constant-time comparison of two `Uint8Array`s. Returns `false` immediately if lengths differ (length check is not constant-time across different lengths, but is across same length — matches `crypto.timingSafeEqual` in Node.js).

**Not included in 0.3.2:** password hashing/verification (argon2id lives in `src/kernel/auth/crypto.hpp` and is auth-kernel-only — extensions MUST NOT roll their own auth), HMAC (can add later if a concrete need appears), asymmetric crypto.

---

## Type Conversion Contract

Every host-registered function follows one consistent argument-checking pattern. Violations produce JS exceptions, never C++ crashes.

### JS → C++ coercion rules

| JS value | C++ extraction | Error on mismatch |
|---|---|---|
| `string` (arg position that expects string) | `std::string` (UTF-8; lone surrogates preserved per QuickJS default) | `TypeError: expected string at arg N` |
| `number` → integer slot (e.g. `randomBytes(n)`) | `int` / `int64_t`; non-finite or non-integer → error | `TypeError: expected integer at arg N` |
| `number` out of declared range | caller-specific bound | `RangeError: <argname> out of range [lo, hi]` |
| `boolean` slot | `bool` | `TypeError: expected boolean at arg N` |
| `Uint8Array` slot | `std::span<const uint8_t>` view over the typed-array buffer | `TypeError: expected Uint8Array at arg N` |
| `object` slot (`log.*` ctx) | recursively converted to `Json::Value` (same helper used by the existing capability dispatcher) | `TypeError` only if non-object supplied where object required |
| missing optional arg | default or omission | — |
| extra args | silently ignored (match Node.js convention — still log at `log.debug` level in a 0.3.2 debug flag, optional) | — |

### C++ → JS conversion rules

| C++ value | JS value |
|---|---|
| `void` / no return | `undefined` |
| `std::string` | `string` (UTF-8 → QuickJS string) |
| `int` / `int64_t` | `number` (if it fits in IEEE 754 double without loss; otherwise throw `RangeError` — 0.3.2 does not introduce BigInt) |
| `bool` | `boolean` |
| `std::vector<uint8_t>` / `std::span<const uint8_t>` | fresh `Uint8Array` (owned copy — no aliasing back to the host buffer) |
| `Json::Value` | structural `JSValue` (reuses the same converter the capability dispatcher already uses) |

---

## Function Registration Mechanism

A single internal helper registers one function:

```cpp
// src/kernel/js/stdlib_inject.hpp
namespace plinth::js {

using HostFn = JSValue (*)(JSContext*, JSValueConst, int, JSValueConst*);

// Register a global function reachable from JS as `namespace.name`.
// Concretely: JS_SetPropertyStr on a fresh or existing object placed on globalThis.
void inject_sync_fn(JSContext* ctx, const char* ns, const char* name, HostFn fn);

// Called once per runtime at pool-creation time (called from RuntimePool ctor).
// Registers the complete 0.3.2 surface.
void inject_kernel_stdlib(JSContext* ctx);

}  // namespace plinth::js
```

One file per namespace under `src/kernel/js/stdlib/`: `log_bindings.cpp`, `config_bindings.cpp`, `crypto_bindings.cpp`. This matches the SESSION-GUIDE §Rules "one file per capability handler" convention, applied to JS kernel injection.

---

## Error Model

Host function misuse from JS raises a JS exception. Four distinct shapes:

| Condition | Exception |
|---|---|
| Wrong `typeof` on any argument | `TypeError` with `"expected <type> at arg <N>"` message |
| Numeric arg out of declared bound (e.g. `randomBytes(-1)`) | `RangeError` with `"<argname> out of range [lo, hi]"` message |
| Invalid enum value (e.g. `crypto.hash("md5", ...)`) | `RangeError` with `"unsupported algorithm: <value>"` message |
| OpenSSL / host failure | `Error` with a short diagnostic |

None of these propagate back to C++ as `EvalErrorKind::INTERNAL` — they stay within JS as regular throwables, catchable by JS `try/catch`. That matters because 0.3.3's async bridge will expect JS exceptions from these functions to be reportable inside a `.catch()` block, same as async rejections.

---

## Performance Targets

Measured on the CI builder image, hot-path (runtime pre-initialized). Benchmarks not required for 0.3.2 — informational targets used during review.

- `log.info("hello world")` round trip: **≤ 10 μs**.
- `config.get("node_id")` round trip: **≤ 5 μs**.
- `crypto.hash("sha256", <1 KiB string>)`: **≤ 100 μs**.
- `crypto.randomBytes(32)`: **≤ 50 μs**.

---

## Security Constraints (Non-Negotiable)

1. `config.get` MUST only return values listed in the hard-coded projection table above. The projection is a compile-time array of `{key, extractor}` pairs. Adding a key is a source change. There is no wildcard, no prefix match, no `"*"`, no runtime-editable allowlist.
2. The projection MUST NOT expose: any `Config::Database` field, `Config::migrations_dir`, or any future field flagged as secret. Reviewers of any PR touching `config_bindings.cpp` MUST audit additions against this rule.
3. `crypto.randomBytes(n)` MUST bound `n ∈ [1, 4096]`. Larger allocations, even for legitimate use, are gated by a future async/streaming API (not in 0.3.x).
4. `crypto.hash` MUST accept only the enumerated algorithms (`sha256`, `sha512`). `md5`, `sha1`, and any other algorithm value produces `RangeError`. Extensions cannot call weak hashes via this API even if OpenSSL exposes them.
5. Host functions MUST NOT read the JS runtime's `BridgeContext` in any way that escalates privilege. In particular, `log.*` MUST NOT auto-inject the caller's extension-id or `user_id` from `BridgeContext` — those are fields the caller would have to supply. Auto-injection of caller identity belongs to the audit API (0.3.3) with a non-forgeable provenance path. **Test (0.3.3.3):** `tests/kernel/js/stdlib_test.cpp → "stdlib: log.* preserves caller-supplied ctx and does not inject kernel fields"` asserts caller-supplied `extension_id` / `user_id` / `node_id` survive verbatim and the kernel `ConfigProjection::node_id` is not spliced in.
6. No host function in 0.3.2 may call `drogon::app().getDbClient()`, `getFastDbClient()`, or any other async primitive. Doing so would either deadlock (single-threaded QuickJS waiting on an event-loop callback) or materialize the same half-init issue that tripped v0.2.4's audit path (see `project_plinth_state` v0.2.4 post-mortem on `g_audit_ready`).

---

## What Must Not Be Decided Yet

The following are 0.3.3+ scope and MUST NOT be introduced by 0.3.2 code:

- **`db.*`:** `db.query`, `db.exec`, `db.exec({ silent: true })`, `db.batch`. All require the promise↔coroutine bridge and the async-op queue. ROADMAP line "db, log, audit, config" for 0.3.2 is scoped back to "log, config, crypto" in this ICD; `db.*` lands in 0.3.3.
- **`audit.*`:** the primitive `plinth::log::audit` exists in C++ (0.1.7) but writing to it from JS requires either (a) async DB insert, or (b) passing through `g_audit_ready` gating — both interact with the async bridge. Defer to 0.3.3.
- **`cap.call`, `cap.batch`:** explicitly 0.3.4. 0.3.2 does not expose any `cap.*` globals. Extensions calling other extensions is a 0.3.4 feature.
- **`pubsub.*`, `storage.*`, `http.*`:** all async. Later milestones (0.5.x, 0.10.x).
- **`plinth.call()` shape:** left opaque at the bridge per DISCUSSION-streaming-and-media §0. 0.3.2 does NOT place anything on a `plinth` global — only `log`, `config`, `crypto`.
- **Auto-injection of caller identity into `log.*`:** belongs to the audit/capability path with a non-forgeable provenance mechanism; premature addition to `log.*` would conflate plain logging with audit.
- **BigInt support:** integers beyond IEEE 754 safe range throw `RangeError` in 0.3.2. BigInt support — if ever needed — is a future decision driven by a concrete caller.
- **Extra `config.get` keys or nested-path access (e.g. `config.get("runtime.memory_limit_mb")`):** only the keys in the projection table ship in 0.3.2. Adding a key or a nested path is a code-session decision per-PR, never a runtime decision.

---

## Milestone Criteria

All four test groups below MUST pass under Catch2 before 0.3.2 ships. They mirror the bullets in `DESIGN-quickjs-bridge.md §9.2` and layer in the security-constraint coverage this ICD adds.

### Tests

1. **`log.info("hello")` surfaces in the kernel log.** Eval `log.info("hello")`; Catch2 assertion inspects spdlog's sink or the logging test-capture shim and confirms `"hello"` appears at INFO level. Same test repeated for `debug`, `warn`, `error` (parameterized). Passing a second `ctx` arg (`{ extension_id: "test" }`) appends a single-line JSON context suffix.
2. **`config.get` returns allowed keys; rejects secret-flagged ones.**
   - `config.get("node_id")` → string equal to the configured node id (test config).
   - `config.get("dev_mode")` → boolean (true in test config).
   - `config.get("unknown_key")` → `null`.
   - `config.get("db.password")` → `null` (the key is outside the projection table — the same path an unknown key takes; NOT a thrown exception, since "unknown" and "excluded" are both legitimate "not in projection" outcomes).
   - `config.get(42)` → throws `TypeError`.
3. **`crypto.hash` correctness + algorithm whitelist.**
   - `crypto.hash("sha256", "")` → `"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"`.
   - `crypto.hash("sha512", "abc")` → known SHA-512 test vector.
   - `crypto.hash("md5", "x")` → throws `RangeError`.
   - `crypto.hash("sha256", new Uint8Array([0x61, 0x62, 0x63]))` → same hash as `"abc"`.
   - `crypto.hash("sha256")` → throws `TypeError` (missing arg).
4. **`crypto.randomBytes` + `crypto.timingSafeEqual`.**
   - `crypto.randomBytes(32)` returns a `Uint8Array` of length 32.
   - Two independent calls produce different outputs with overwhelming probability (compare byte-for-byte; a fail threshold of > 1-in-2^128 is fine).
   - `crypto.randomBytes(0)` → throws `RangeError`.
   - `crypto.randomBytes(5000)` → throws `RangeError`.
   - `crypto.timingSafeEqual(new Uint8Array([1,2,3]), new Uint8Array([1,2,3]))` → `true`.
   - `crypto.timingSafeEqual(new Uint8Array([1,2,3]), new Uint8Array([1,2,4]))` → `false`.
   - `crypto.timingSafeEqual(new Uint8Array([1,2,3]), new Uint8Array([1,2]))` → `false` (different lengths).

### CI Wiring

- `src/kernel/js/stdlib_inject.{hpp,cpp}` + one file per namespace under `src/kernel/js/stdlib/`.
- Test file at `tests/kernel/js/stdlib_test.cpp` registered in the existing Catch2 executable.
- `inject_kernel_stdlib(ctx)` called from `RuntimePool` context construction — every pooled runtime carries the stdlib from 0.3.2 onward.
- No new CI job.
