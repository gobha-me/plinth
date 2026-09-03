#pragma once

// plinth::capabilities — capability dispatch / resolution (ICD-0.2.2).
//
// Translates a "namespace:version:function" signature (plus args + user
// context) into a handler invocation following the three-tier algorithm:
//
//   Tier 1  → in-process map of direct C++ handlers (kernel caps only).
//   Tier 2  → in-memory snapshot of plinth.capabilities; loaded at
//             startup, refreshed by the LISTEN/NOTIFY listener (0.2.3).
//   Tier 3  → remote / sidecar proxy. Stub in 0.2.x — returns
//             tier3_not_available; real impl in milestone 0.8.
//
// ── Deviations from ICD-0.2.2 (mirrors the 0.2.0 accepted deviation)
//
//   1. Async dispatch wrapper: see ICD-0.2.6-async-dispatch.md for the
//      coroutine-shaped entry point (call_capability_async /
//      batch_call_capability_async) consumed by 0.3.3+. The synchronous
//      call_capability form defined below remains the primary
//      implementation; the async wrapper composes on top of it.
//
//   2. Tier 1 handlers are stubs in 0.2.2. The five kernel capabilities
//      (kernel:1:db.query, db.exec, log, audit, config.get) register
//      echo handlers that return {"not_implemented": "<signature>"}.
//      Real wiring lands when a real caller first demands it.
//
//   3. RBAC step 3 is enforced in 0.2.4: the resolver consults the
//      `rbac_rule` carried by the Tier 1 / Tier 2 entry against the
//      caller's effective_rules (plus the `kernel.admin` universal
//      match). No DB fallback — a cache miss resolves as
//      capability_not_found, consistent with 0.2.2's precedent. See
//      ICD-0.2.4-capability-rbac.md.
//
// All state is module-local in resolution.cpp; this header exposes only
// the types callers use and three test helpers guarded by a clear
// "_for_test" naming convention.

#include "kernel/capabilities/types.hpp"
#include "kernel/config.hpp"

#include <drogon/utils/coroutine.h>
#include <expected>
#include <functional>
#include <json/value.h>
#include <string>
#include <vector>

namespace plinth::capabilities {

// Default maximum call depth per ICD-0.2.2 §Call Depth Tracking.
inline constexpr int MAX_CALL_DEPTH = 8;

// ── Public dispatch types ────────────────────────────────────────────

// Input to call_capability(). `call_depth` starts at 0 for the originating
// request and is incremented once per hop through the resolver.
struct CapabilityCall {
  std::string signature; // "namespace:version:function"
  Json::Value args;      // forwarded to the handler verbatim
  int call_depth = 0;    // per-request hop counter
};

// Successful dispatch payload.
struct CapabilityResult {
  Json::Value data;          // handler return value
  std::string resolved_tier; // "tier1" | "tier2" | "tier3"
  std::string provider_type; // "kernel" | "extension" | "sidecar"
};

// Ambient user context carried through the dispatch pipeline.
//
// `user_id` drives scope precedence (user-scope Tier 2 beats instance-
// scope for the calling user). `effective_rules` drives the 0.2.4 RBAC
// check at step 3 — callers must pre-populate this vector (HTTP callers
// copy it out of the request attribute set by plinth::rbac::RbacFilter;
// tests populate it directly). `session_id` / `ip_address` are purely
// for audit-event enrichment and may be empty (→ SQL NULL).
//
// `auth_type == "anonymous"` is the sentinel produced by anonymous()
// (architecture/01-identity.md §3) — an unauthenticated context whose
// effective_rules are the union of rules granted to the `everyone`
// group. Reserved for future paths that bypass SessionFilter; no live
// request path synthesizes it today.
struct UserContext {
  std::string user_id;
  std::string username;
  std::string auth_type;                    // "session" | "pat" | "anonymous"
  std::vector<std::string> effective_rules; // additive union (ICD-0.1.5)
  std::string session_id;                   // empty → NULL in audit row
  std::string ip_address;                   // empty → NULL in audit row

  // Baseline anonymous identity per architecture/01-identity.md §3.
  // user_id is empty (the sentinel for "null — not a real user"),
  // auth_type is "anonymous", and effective_rules is empty. Production
  // callers that synthesize an anonymous context should fetch the
  // rules granted to the `everyone` group from plinth.group_rules and
  // use anonymous_with_rules() to populate the vector. The enforcement
  // test (tests/kernel/rbac/anonymous_identity_test.cpp) asserts that
  // this baseline is rejected by every RBAC-gated route — the
  // "permanent safeguard against accidental public exposure" called
  // for by §3.
  [[nodiscard]] static auto anonymous() -> UserContext {
    return UserContext{.user_id = "",
                       .username = "",
                       .auth_type = "anonymous",
                       .effective_rules = {},
                       .session_id = "",
                       .ip_address = ""};
  }

  // Anonymous identity with a caller-supplied effective-rules vector.
  // Typical production use: pass the union of rules granted to the
  // `everyone` group. Used by the 0.2.6.1 test to assert that
  // granting a rule to `everyone` unlocks anonymous access to a
  // route that requires it.
  [[nodiscard]] static auto anonymous_with_rules(std::vector<std::string> rules)
      -> UserContext {
    auto ctx = anonymous();
    ctx.effective_rules = std::move(rules);
    return ctx;
  }
};

// Outcome of call_capability(). Matches the RegisterResult pattern
// in types.hpp — `std::expected` gives us the "exactly one arm engaged"
// invariant for free.
using ResolveResult = std::expected<CapabilityResult, CapabilityError>;

// Signature of a Tier 1 handler. Handlers run synchronously and return
// either a JSON payload or a capability error.
using HandlerOutcome = std::expected<Json::Value, CapabilityError>;
using CapabilityHandler = std::function<HandlerOutcome(
    const Json::Value& args, const UserContext& ctx, int call_depth)>;

// ── Public API ───────────────────────────────────────────────────────

// One-shot startup wiring (idempotent within a process): registers the
// kernel Tier 1 stub handlers and loads every enabled row from
// plinth.capabilities into the Tier 2 cache via sync libpq. Called from
// main.cpp after bootstrap_kernel_capabilities and before app().run().
auto init_resolver(const Config::Database& db_cfg) -> void;

// The dispatch entry point (ICD-0.2.2 §Resolution Algorithm). Pure
// function of (call, ctx, resolver state): no DB access on the hot path.
auto call_capability(const CapabilityCall& call, const UserContext& ctx)
    -> ResolveResult;

// Async (coroutine) dispatch entry — see ICD-0.2.6-async-dispatch.md
// and ICD-0.5.0.3 §Resolver integration. For Tier 1 handlers and
// Tier 2 non-extension entries this composes trivially on
// call_capability. For Tier 2 `provider_type == "extension"` entries
// it co_awaits `plinth::extensions::dispatch` after resolving the
// entry + RBAC under the resolver's shared lock and releasing it.
//
// ICD-0.5.0.3 §Error taxonomy — `CapabilityError::EXTENSION_DISPATCH_FAILED`
// is the enum-side transport when the extension dispatch returns a
// `PromiseRejection`. The concrete `cap.*` code + capped message
// propagate through the optional `ext_detail_code_out` +
// `ext_detail_message_out` pointers: when non-null and the return is
// `EXTENSION_DISPATCH_FAILED`, they are populated with the
// PromiseRejection fields (e.g. `"cap.handler_threw"` + the capped
// message) so downstream mappers like
// `plinth::js::capability_error_to_rejection` can surface them to JS
// callers without flattening the taxonomy. Callers that don't need
// the detail may pass nullptr (or omit — the defaults are nullptr).
//
// reference arguments are read-only and outlive the coroutine by caller
// contract (`run_cap_call_outcome` owns the moved op; WS handlers own ctx on
// the dispatch frame). Copying CapabilityCall would defeat the "no behavior
// change" guarantee for the sync lane.
auto call_capability_async(const CapabilityCall& call, const UserContext& ctx,
                           std::string* ext_detail_code_out = nullptr,
                           std::string* ext_detail_message_out = nullptr)
    -> drogon::Task<ResolveResult>;

// Full Tier 2 cache resync from plinth.capabilities. Takes the
// resolver write lock, clears tier2_cache, and re-runs the `enabled =
// true` load. Returns the number of rows loaded.
//
// Cache-invalidation policy (0.2.4): LISTEN/NOTIFY is the primary
// channel, but a NOTIFY delivered during a reconnect backoff window
// is lost. The listener calls this helper after every successful
// LISTEN open so missed-NOTIFY recovery is bounded by reconnect
// backoff (≤ 1 s) plus one SELECT, not by process lifetime. Tier 1
// handlers are untouched.
auto reload_tier2_cache(const Config::Database& db_cfg) -> std::size_t;

// ── Test / bootstrap helpers ─────────────────────────────────────────
//
// Deliberately exposed so tests can drive the resolver without a PG
// instance. Production code must not call these after init_resolver()
// returns except from kernel bootstrap.

// Replace or insert a Tier 1 handler, bound to its required RBAC rule.
// The rule is checked at step 3 of call_capability before the handler
// ever runs (ICD-0.2.4 §Permission Check in Dispatch Pipeline). An
// empty `rbac_rule` is treated as fail-closed — the call is denied —
// which keeps stray registrations from silently accepting any caller.
auto register_tier1_handler(std::string signature, std::string rbac_rule,
                            CapabilityHandler handler) -> void;

// A single Tier 2 cache entry (mirrors the schema columns the resolver
// consults; no `id` / `registered_at` etc.).
struct CachedCapability {
  std::string signature;
  std::string provider_type;  // "kernel" | "extension" | "sidecar"
  std::string extension_name; // empty for kernel
  std::string scope;          // "instance" | "user"
  std::string user_id;        // populated only for scope="user"
  std::string rbac_rule;
  bool enabled = true;
};

// Insert or replace a single Tier 2 cache entry. Key derivation
// matches the algorithm used by call_capability (instance scope vs
// "<signature>:user:<uid>" for user scope). Called by the NOTIFY
// listener (0.2.3) on "register" events and by tests.
auto upsert_tier2_entry(const CachedCapability& entry) -> void;

// Remove a single Tier 2 cache entry by (signature, scope). No-op
// if the entry does not exist. Called by the NOTIFY listener on
// "deregister" events.
auto erase_tier2_entry(std::string_view signature, std::string_view scope)
    -> void;

// Flip the `enabled` flag on every Tier 2 cache entry whose
// extension_name matches. Returns the number of entries updated
// (informational — the listener logs it at debug). Called by the
// NOTIFY listener on "disable" / "enable" events when the payload
// carries extension_name and an empty signature (bulk scope).
auto set_enabled_by_extension_in_cache(std::string_view extension_name,
                                       bool enabled) -> std::size_t;

// Seed (or overwrite) a Tier 2 cache entry. Thin wrapper around
// upsert_tier2_entry kept for test clarity — the `_for_test` suffix
// advertises the intent at the call site.
auto seed_tier2_cache_for_test(const CachedCapability& entry) -> void;

// Empty Tier 1 map and Tier 2 cache. Tests call this between cases.
auto clear_resolver_for_test() -> void;

// Current Tier 2 cache size. Used by the startup smoke-log in
// init_resolver and by tests to assert load counts.
auto tier2_cache_size() -> std::size_t;

} // namespace plinth::capabilities
