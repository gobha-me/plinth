#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/parser.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/logging.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::capabilities {

namespace {

// ── Module-local state ───────────────────────────────────────────────
//
// Two maps behind a shared_mutex. Dispatch takes a shared (read) lock;
// the future NOTIFY listener (0.2.3) and the test helpers take the
// unique (write) lock. Both maps are tiny (tens to low-hundreds of
// entries) so lock contention is not a concern for 0.2.x.
//
// tier1: key = canonical signature ("kernel:1:db.query").
// tier2: key = signature for instance scope, or signature + ":user:<uid>"
//        for user scope. Instance + user entries for the same signature
//        coexist; call_capability checks the user key first.

// A Tier 1 entry pairs the handler with its required RBAC rule. The
// rule lives alongside the handler so the step-3 check (ICD-0.2.4) can
// look up both in a single dispatch-lock window.
struct Tier1Entry {
  CapabilityHandler handler;
  std::string rbac_rule;
};

// protecting the two maps below; cannot be const (lock() mutates).
std::shared_mutex state_mutex;
// state is inherently process-wide; populated at startup, mutated by NOTIFY
// refresh (0.2.3) and test helpers only.
std::unordered_map<std::string, Tier1Entry> tier1_map;
// state is inherently process-wide; populated at startup, mutated by NOTIFY
// refresh (0.2.3) and test helpers only.
std::unordered_map<std::string, CachedCapability> tier2_cache;

auto user_scope_key(std::string_view signature, std::string_view user_id)
    -> std::string {
  std::string k;
  k.reserve(signature.size() + 6 + user_id.size());
  k.append(signature);
  k.append(":user:");
  k.append(user_id);
  return k;
}

// ── Small helpers for building outcomes ──────────────────────────────
//
// `failure()` wraps std::unexpected for call-site locality. The matching
// `success()` helper is not needed: `return CapabilityResult{...}` relies
// on std::expected's implicit T-to-expected<T,E> conversion.

auto failure(CapabilityError e) -> ResolveResult {
  return std::unexpected(e);
}

// ── Tier 1 stub handlers (ICD-0.2.2 §Milestone Criteria 0.2.2) ───────
//
// The five kernel capabilities bootstrapped by
// plinth::capabilities::bootstrap_kernel_capabilities must all be
// resolvable at Tier 1. For 0.2.2 the handlers are intentionally stubs:
// they prove the dispatch path end-to-end without pulling the kernel's
// db / log / audit / config internals into this module. The real
// handlers land with the first real caller (JS bridge in 0.3.x).

auto make_not_implemented_handler(const char* signature) -> CapabilityHandler {
  return [signature](const Json::Value& /*args*/, const UserContext& /*ctx*/,
                     int /*call_depth*/) -> HandlerOutcome {
    Json::Value payload(Json::objectValue);
    payload["not_implemented"] = signature;
    return payload;
  };
}

// Signature + rbac_rule pairs kept in sync with bootstrap.cpp
// KERNEL_CAPS. Duplicated (rather than lifted into a shared header)
// because the two lists have different shapes: bootstrap needs
// description for the DB seed, the resolver only needs signature +
// rule. Keeping them side-by-side is noisier than it is fragile — a
// mismatch is caught by the 0.2.2 smoke test (init_resolver logs
// "tier1_handlers=5") and by the capability.registered bootstrap
// audit row. Updated for 0.2.4 to carry rbac_rule for step-3
// enforcement.
struct KernelStub {
  const char* signature;
  const char* rbac_rule;
};
constexpr std::array<KernelStub, 5> KERNEL_STUBS = {
    KernelStub{.signature = "kernel:1:db.query",
               .rbac_rule = "kernel.db.query"},
    KernelStub{.signature = "kernel:1:db.exec", .rbac_rule = "kernel.db.exec"},
    KernelStub{.signature = "kernel:1:log", .rbac_rule = "kernel.log"},
    KernelStub{.signature = "kernel:1:audit", .rbac_rule = "kernel.audit"},
    KernelStub{.signature = "kernel:1:config.get",
               .rbac_rule = "kernel.config.get"},
};

auto register_kernel_stubs_locked() -> void {
  for (const auto& stub : KERNEL_STUBS) {
    tier1_map[stub.signature] = Tier1Entry{
        .handler = make_not_implemented_handler(stub.signature),
        .rbac_rule = stub.rbac_rule,
    };
  }
}

// ── Load harness (LH-0) Tier 1 handler ───────────────────────────────
//
// `lh0:1:chain` recursively invokes itself via the standard resolver
// pipeline, exercising the sync dispatch path (Tier 1 lookup + RBAC
// check + call-depth tracking) at depth N. Used by the external load
// harness (docs/icd/ICD-LH-0-load-harness-scaffold.md) to saturate
// the cap.call dispatch path under sustained WS load.
//
// Signature: `lh0:1:chain`. Argument: positional `[depth:int]`
// (missing / non-int treated as 1 → terminal). Returns:
// `{"depth": N, "sub": <child result> | {terminal: true}}`.
//
// RBAC: gated by `kernel.admin` — the universal-match rule. Non-admin
// callers receive `permission_denied` at step 3 of the resolver. This
// capability is not advertised to extensions (it lives only in the
// Tier 1 in-memory map, never in `plinth.capabilities`).
//
// Depth > MAX_CALL_DEPTH surfaces as `call_depth_exceeded` via the
// resolver's Step 2 check on the recursive invocation — not something
// the handler itself enforces.
auto lh0_chain_handler(const Json::Value& args, const UserContext& ctx,
                       int call_depth) -> HandlerOutcome {
  int depth = 1;
  if (args.isArray() && !args.empty() && args[0].isInt()) {
    depth = args[0].asInt();
  }

  if (depth <= 1) {
    Json::Value out(Json::objectValue);
    out["depth"] = depth <= 0 ? 1 : depth;
    out["terminal"] = true;
    return out;
  }

  Json::Value sub_args(Json::arrayValue);
  sub_args.append(depth - 1);
  CapabilityCall sub{
      .signature = "lh0:1:chain",
      .args = std::move(sub_args),
      .call_depth = call_depth,
  };
  auto res = call_capability(sub, ctx);
  if (!res.has_value()) {
    return std::unexpected(res.error());
  }
  Json::Value out(Json::objectValue);
  out["depth"] = depth;
  out["sub"] = std::move(res->data);
  return out;
}

auto register_lh0_harness_handlers_locked() -> void {
  tier1_map["lh0:1:chain"] = Tier1Entry{
      .handler = lh0_chain_handler,
      .rbac_rule = "kernel.admin",
  };
}

// ── Tier 2 cache load via sync libpq ─────────────────────────────────

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

// Pulls every `enabled = true` row. Disabled rows are intentionally
// excluded: dispatch resolves strictly against the enabled snapshot,
// and the disable/enable audit trail plus the NOTIFY refresh (0.2.3)
// keeps cache membership in sync. For tests that want to exercise the
// "capability_disabled" branch, seed_tier2_cache_for_test accepts
// enabled=false directly.
//
// Called under the state_mutex write lock from both init_resolver
// (one-shot startup) and reload_tier2_cache (listener reconnect resync
// — 0.2.4). The caller is responsible for clearing tier2_cache first
// if a full refresh is required; this function only inserts.
auto load_tier2_cache_locked(const Config::Database& db_cfg) -> std::size_t {
  auto conninfo = build_conninfo(db_cfg);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    spdlog::error("tier2 load: PG connect failed: {}", err);
    return 0;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);

  PgResultPtr res{PQexec(conn, "SELECT signature, provider_type, "
                               "       COALESCE(extension_name, ''), scope, "
                               "       rbac_rule "
                               "FROM plinth.capabilities "
                               "WHERE enabled = true"),
                  PQclear};
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::error("tier2 load: SELECT failed: {}",
                  PQresultErrorMessage(res.get()));
    return 0;
  }

  std::size_t inserted = 0;
  int rows = PQntuples(res.get());
  for (int i = 0; i < rows; ++i) {
    CachedCapability entry{
        .signature = PQgetvalue(res.get(), i, 0),
        .provider_type = PQgetvalue(res.get(), i, 1),
        .extension_name = PQgetvalue(res.get(), i, 2),
        .scope = PQgetvalue(res.get(), i, 3),
        .user_id = {}, // user-scope deferred to 0.4.x
        .rbac_rule = PQgetvalue(res.get(), i, 4),
        .enabled = true,
    };
    // Only instance-scope rows exist in 0.2.x (per ICD-0.2.0
    // user-scope deferral). The user_scope_key path is still
    // exercised by unit tests via seed_tier2_cache_for_test.
    tier2_cache[entry.signature] = std::move(entry);
    ++inserted;
  }
  return inserted;
}

// ── Tier-specific dispatch ───────────────────────────────────────────

auto dispatch_tier1(const Tier1Entry& entry, const CapabilityCall& call,
                    const UserContext& ctx) -> ResolveResult {
  auto out = entry.handler(call.args, ctx, call.call_depth + 1);
  if (!out.has_value()) {
    return failure(out.error());
  }
  return CapabilityResult{
      .data = std::move(*out),
      .resolved_tier = "tier1",
      .provider_type = "kernel",
  };
}

// ── Step 3: RBAC enforcement (ICD-0.2.4) ─────────────────────────────
//
// `check_permission` is the additive-union check from ICD-0.1.5 lifted
// to an in-memory vector: a user is granted if the required rule is
// present OR if they hold the universal `kernel.admin` rule. Empty
// `required_rule` short-circuits to deny (fail-closed per
// ICD-0.2.4 §Security Constraint 2).

constexpr std::string_view KERNEL_ADMIN_RULE = "kernel.admin";

auto check_permission(const std::vector<std::string>& effective_rules,
                      std::string_view required_rule) -> bool {
  if (required_rule.empty()) {
    return false;
  }
  return std::ranges::any_of(
      effective_rules, [required_rule](const std::string& rule) {
        return rule == required_rule || rule == KERNEL_ADMIN_RULE;
      });
}

// Fire-and-forget audit of a capability RBAC denial. Shape matches
// ICD-0.2.4 §Audit Events. Safe from any thread — `plinth::log::audit`
// queues the insert through Drogon's DbClient.
auto emit_rbac_denied(const CapabilityCall& call, const UserContext& ctx,
                      std::string_view rule) -> void {
  Json::Value detail(Json::objectValue);
  detail["capability"] = call.signature;
  detail["rule"] = std::string{rule};
  detail["call_depth"] = call.call_depth;
  if (!ctx.username.empty()) {
    detail["username"] = ctx.username;
  }
  plinth::log::audit("capability.rbac.denied", detail,
                     plinth::log::AuditCtx{
                         .user_id = ctx.user_id,
                         .session_id = ctx.session_id,
                         .ip_address = ctx.ip_address,
                     });
}

// Sync-path Tier 2 dispatch. Per ICD-0.5.0.3 §Sync vs async the
// extension arm now rejects with ASYNC_REQUIRED — extension dispatch
// needs JS evaluation, which only composes cleanly with the coroutine
// resolver. Sidecar stays at TIER3_NOT_AVAILABLE pending 0.8.x.
auto dispatch_tier2(const CachedCapability& entry,
                    const CapabilityCall& /*call*/, const UserContext& /*ctx*/)
    -> ResolveResult {
  if (!entry.enabled) {
    return failure(CapabilityError::CAPABILITY_DISABLED);
  }
  if (entry.provider_type == "sidecar") {
    // Known-remote, but the sidecar tier is not built yet (ICD-0.2.2
    // §Resolution Algorithm step 6, milestone 0.8).
    return failure(CapabilityError::TIER3_NOT_AVAILABLE);
  }
  if (entry.provider_type == "extension") {
    // ICD-0.5.0.3 §Error taxonomy. Sync callers that land an
    // extension entry must migrate to `call_capability_async`; the
    // JS-side path always has. WS `on_call` migrated in 0.5.0.4.
    return failure(CapabilityError::ASYNC_REQUIRED);
  }
  // provider_type == "kernel": every kernel cap should have been
  // handled by Tier 1 already. Reaching this arm means someone loaded
  // a kernel row into the cache without registering a Tier 1 handler
  // — a configuration mistake, not a user error.
  spdlog::warn("Tier 2 hit for kernel provider ({}); expected Tier 1",
               entry.signature);
  return failure(CapabilityError::CAPABILITY_NOT_FOUND);
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

auto init_resolver(const Config::Database& db_cfg) -> void {
  std::unique_lock lock(state_mutex);
  tier1_map.clear();
  tier2_cache.clear();
  register_kernel_stubs_locked();
  register_lh0_harness_handlers_locked();
  auto loaded = load_tier2_cache_locked(db_cfg);
  spdlog::info("capability resolver initialized: tier1_handlers={} "
               "tier2_entries={}",
               tier1_map.size(), loaded);
}

auto reload_tier2_cache(const Config::Database& db_cfg) -> std::size_t {
  std::unique_lock lock(state_mutex);
  tier2_cache.clear();
  auto loaded = load_tier2_cache_locked(db_cfg);
  spdlog::info("tier2 cache resynced: entries={}", loaded);
  return loaded;
}

auto call_capability(const CapabilityCall& call, const UserContext& ctx)
    -> ResolveResult {
  // Step 1 — parse + validate signature (ICD §Resolution Algorithm 1).
  auto parsed = parse_signature(call.signature);
  if (std::holds_alternative<CapabilityError>(parsed)) {
    // Any parser error maps to the caller-visible
    // "invalid_capability" per ICD-0.2.2 §Error Codes. The inner
    // enum variant (INVALID_NAMESPACE etc.) is not leaked.
    return failure(CapabilityError::INVALID_CAPABILITY);
  }

  // Step 2 — call depth check.
  if (call.call_depth >= MAX_CALL_DEPTH) {
    return failure(CapabilityError::CALL_DEPTH_EXCEEDED);
  }

  // ICD-0.4.5 §T1 — count this dispatch against any active upgrade
  // drain for the extension. Cheap when no drain is active: one
  // relaxed atomic read inside the guard ctor.
  drain::DispatchGuard drain_guard(
      std::get<ParsedSignature>(parsed).namespace_);

  // Steps 3–5 — resolve the candidate entry under a single shared
  // lock, run the RBAC check against its rbac_rule, then dispatch.
  // Per ICD-0.2.4 §Rule Lookup Ordering, rule lookup and dispatch
  // share the same lookup; we walk Tier 1 then Tier 2 (with scope
  // precedence) once and pull the rule off whichever matched.
  //
  // A cache miss short-circuits to capability_not_found before RBAC
  // runs — we never audit "permission_denied" for a signature we
  // cannot identify, consistent with the ICD denial shape (which
  // requires a concrete `rule` field).

  std::shared_lock lock(state_mutex);

  const Tier1Entry* tier1_hit = nullptr;
  const CachedCapability* tier2_hit = nullptr;
  std::string_view required_rule;

  if (auto it = tier1_map.find(call.signature); it != tier1_map.end()) {
    tier1_hit = &it->second;
    required_rule = it->second.rbac_rule;
  } else {
    if (!ctx.user_id.empty()) {
      auto ukey = user_scope_key(call.signature, ctx.user_id);
      if (auto it2 = tier2_cache.find(ukey); it2 != tier2_cache.end()) {
        tier2_hit = &it2->second;
        required_rule = it2->second.rbac_rule;
      }
    }
    if (tier2_hit == nullptr) {
      if (auto it2 = tier2_cache.find(call.signature);
          it2 != tier2_cache.end()) {
        tier2_hit = &it2->second;
        required_rule = it2->second.rbac_rule;
      }
    }
  }

  if (tier1_hit == nullptr && tier2_hit == nullptr) {
    // Step 6 — unknown signature. 0.2.2 precedent: capability_not_
    // found, not permission_denied (we have no rule to report).
    return failure(CapabilityError::CAPABILITY_NOT_FOUND);
  }

  // Step 3 — RBAC check (ICD-0.2.4). An empty rbac_rule on a
  // cache-hit entry is fail-closed — check_permission returns false
  // for required_rule.empty().
  if (!check_permission(ctx.effective_rules, required_rule)) {
    emit_rbac_denied(call, ctx, required_rule);
    return failure(CapabilityError::PERMISSION_DENIED);
  }

  // Step 4/5 — dispatch the entry we already resolved.
  if (tier1_hit != nullptr) {
    return dispatch_tier1(*tier1_hit, call, ctx);
  }
  return dispatch_tier2(*tier2_hit, call, ctx);
}

// header: arguments outlive the coroutine; copying defeats the resolver's "no
// behavior change" guarantee for the sync-lane portion of this path.
auto call_capability_async(const CapabilityCall& call, const UserContext& ctx,
                           std::string* ext_detail_code_out,
                           std::string* ext_detail_message_out)
    -> drogon::Task<ResolveResult> {
  // The async resolver mirrors `call_capability`'s steps 1-5 verbatim,
  // then diverges at step 8 for `provider_type == "extension"` — the
  // extension arm releases the resolver's shared_lock and co_awaits
  // `plinth::extensions::dispatch`. Tier 1 + kernel/sidecar Tier 2
  // dispatches run under the lock, identical to the sync path.
  //
  // Duplicating steps 1-5 (rather than factoring them into a helper)
  // keeps the two entry points independently readable; the helper
  // would need to return a shared_lock by value through the
  // expected-like boundary, which complicates the TU. 0.5.0.4 ships
  // the duplication; a later refactor may collapse.

  // Step 1 — parse.
  auto parsed = parse_signature(call.signature);
  if (std::holds_alternative<CapabilityError>(parsed)) {
    co_return failure(CapabilityError::INVALID_CAPABILITY);
  }

  // Step 2 — call depth.
  if (call.call_depth >= MAX_CALL_DEPTH) {
    co_return failure(CapabilityError::CALL_DEPTH_EXCEEDED);
  }

  // ICD-0.4.5 §T1 drain guard — see sync path. Lives on the coroutine
  // frame for the duration of the dispatch.
  drain::DispatchGuard drain_guard(
      std::get<ParsedSignature>(parsed).namespace_);

  // Steps 3-5 under the shared lock: Tier 1 / Tier 2 lookup + RBAC.
  // For extension entries we extract identity + release the lock
  // before the co_await; for everything else we dispatch under the
  // lock and co_return the result.
  std::string ext_name;
  std::string ext_function;
  bool extension_dispatch = false;
  {
    std::shared_lock lock(state_mutex);

    const Tier1Entry* tier1_hit = nullptr;
    const CachedCapability* tier2_hit = nullptr;
    std::string_view required_rule;

    if (auto it = tier1_map.find(call.signature); it != tier1_map.end()) {
      tier1_hit = &it->second;
      required_rule = it->second.rbac_rule;
    } else {
      if (!ctx.user_id.empty()) {
        auto ukey = user_scope_key(call.signature, ctx.user_id);
        if (auto it2 = tier2_cache.find(ukey); it2 != tier2_cache.end()) {
          tier2_hit = &it2->second;
          required_rule = it2->second.rbac_rule;
        }
      }
      if (tier2_hit == nullptr) {
        if (auto it2 = tier2_cache.find(call.signature);
            it2 != tier2_cache.end()) {
          tier2_hit = &it2->second;
          required_rule = it2->second.rbac_rule;
        }
      }
    }

    if (tier1_hit == nullptr && tier2_hit == nullptr) {
      co_return failure(CapabilityError::CAPABILITY_NOT_FOUND);
    }

    if (!check_permission(ctx.effective_rules, required_rule)) {
      emit_rbac_denied(call, ctx, required_rule);
      co_return failure(CapabilityError::PERMISSION_DENIED);
    }

    if (tier1_hit != nullptr) {
      co_return dispatch_tier1(*tier1_hit, call, ctx);
    }
    // tier2_hit != nullptr below.
    if (tier2_hit->provider_type != "extension") {
      // Kernel-in-tier2 + sidecar paths are unchanged (sync tier2).
      co_return dispatch_tier2(*tier2_hit, call, ctx);
    }
    // Extension entry — capture identity + signature, fall through
    // to the post-lock co_await.
    if (!tier2_hit->enabled) {
      co_return failure(CapabilityError::CAPABILITY_DISABLED);
    }
    ext_name = tier2_hit->extension_name;
    ext_function = std::get<ParsedSignature>(parsed).function;
    extension_dispatch = true;
  }

  if (!extension_dispatch) {
    // Unreachable — every lock-scope exit path above already
    // co_return'd. Guard anyway so the compiler does not warn.
    co_return failure(CapabilityError::CAPABILITY_NOT_FOUND);
  }

  // Post-lock: co_await into the extension pool. The registry owns
  // its own locking; the resolver's shared_lock was released above.
  auto dispatch_out = co_await plinth::extensions::dispatch(
      ext_name, ext_function, call.args, ctx, call.call_depth);

  if (dispatch_out.has_value()) {
    co_return CapabilityResult{
        .data = std::move(*dispatch_out),
        .resolved_tier = "tier2",
        .provider_type = "extension",
    };
  }

  // Populate the out-param detail channel so the JS caller's
  // `capability_error_to_rejection` preserves the cap.* code +
  // capped message instead of collapsing to cap.internal.
  if (ext_detail_code_out != nullptr) {
    *ext_detail_code_out = dispatch_out.error().code;
  }
  if (ext_detail_message_out != nullptr) {
    *ext_detail_message_out = dispatch_out.error().message;
  }
  co_return failure(CapabilityError::EXTENSION_DISPATCH_FAILED);
}

auto register_tier1_handler(std::string signature, std::string rbac_rule,
                            CapabilityHandler handler) -> void {
  std::unique_lock lock(state_mutex);
  tier1_map[std::move(signature)] = Tier1Entry{
      .handler = std::move(handler),
      .rbac_rule = std::move(rbac_rule),
  };
}

auto upsert_tier2_entry(const CachedCapability& entry) -> void {
  std::unique_lock lock(state_mutex);
  std::string key = entry.scope == "user" && !entry.user_id.empty()
                        ? user_scope_key(entry.signature, entry.user_id)
                        : entry.signature;
  tier2_cache[key] = entry;
}

auto erase_tier2_entry(std::string_view signature, std::string_view /*scope*/)
    -> void {
  // 0.2.x persists only instance-scope rows (ICD-0.2.0 user-scope
  // deferral), and the NOTIFY payload for deregister does not carry
  // a user_id. The keyed cache therefore only contains instance keys
  // emitted by the listener, so erasure by bare signature is correct
  // for today and forward-compatible: when user-scope lands in
  // 0.4.x, the payload will gain user_id and this helper can be
  // extended to derive the user key.
  std::unique_lock lock(state_mutex);
  tier2_cache.erase(std::string{signature});
}

auto set_enabled_by_extension_in_cache(std::string_view extension_name,
                                       bool enabled) -> std::size_t {
  std::unique_lock lock(state_mutex);
  std::size_t updated = 0;
  for (auto& kv : tier2_cache) {
    if (kv.second.extension_name == extension_name) {
      kv.second.enabled = enabled;
      ++updated;
    }
  }
  return updated;
}

auto seed_tier2_cache_for_test(const CachedCapability& entry) -> void {
  upsert_tier2_entry(entry);
}

auto clear_resolver_for_test() -> void {
  std::unique_lock lock(state_mutex);
  tier1_map.clear();
  tier2_cache.clear();
}

auto tier2_cache_size() -> std::size_t {
  std::shared_lock lock(state_mutex);
  return tier2_cache.size();
}

} // namespace plinth::capabilities
