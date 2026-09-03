#include <catch2/catch_test_macros.hpp>

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp" // error_code()

#include <drogon/utils/coroutine.h>
#include <expected>
#include <json/value.h>
#include <string>
#include <vector>

// Unit tests for plinth::capabilities::call_capability — pure, no PG
// required. Drives the resolver through register_tier1_handler and
// seed_tier2_cache_for_test so each case exercises exactly one branch
// of the §Resolution Algorithm.
//
// Coverage maps 1:1 to ICD-0.2.2 §Milestone Criteria (0.2.2 Exit):
//   * Tier 1 hit
//   * Tier 2 hit
//   * Tier 2 miss
//   * Disabled capability
//   * Scope override (user-scope beats instance-scope)
//   * Call depth exceeded
//   * Invalid identifier
//   * Tier 3 stub (known-remote provider_type)

using plinth::capabilities::CachedCapability;
using plinth::capabilities::CapabilityCall;
using plinth::capabilities::CapabilityError;
using plinth::capabilities::CapabilityHandler;
using plinth::capabilities::CapabilityResult;
using plinth::capabilities::HandlerOutcome;
using plinth::capabilities::MAX_CALL_DEPTH;
using plinth::capabilities::ResolveResult;
using plinth::capabilities::UserContext;

namespace {

auto make_echo_handler(std::string tag) -> CapabilityHandler {
  return [tag = std::move(tag)](const Json::Value& args,
                                const UserContext& /*ctx*/,
                                int call_depth) -> HandlerOutcome {
    Json::Value payload(Json::objectValue);
    payload["tag"] = tag;
    payload["args"] = args;
    payload["call_depth"] = call_depth;
    return payload;
  };
}

auto fresh_resolver() -> void {
  plinth::capabilities::clear_resolver_for_test();
}

// Default context carries `kernel.admin` so the step-3 RBAC check
// (ICD-0.2.4) is a pass-through in tests that are not specifically
// exercising permission enforcement. Cases that test denial override
// `effective_rules` explicitly via ctx_with_rules().
auto default_ctx() -> UserContext {
  return UserContext{.user_id = "11111111-1111-1111-1111-111111111111",
                     .username = "alice",
                     .auth_type = "session",
                     .effective_rules = {"kernel.admin"},
                     .session_id = {},
                     .ip_address = {}};
}

auto ctx_with_rules(std::vector<std::string> rules) -> UserContext {
  auto ctx = default_ctx();
  ctx.effective_rules = std::move(rules);
  return ctx;
}

auto make_instance_entry(std::string signature, std::string provider_type,
                         bool enabled = true) -> CachedCapability {
  bool is_kernel = provider_type == "kernel";
  return CachedCapability{
      .signature = std::move(signature),
      .provider_type = std::move(provider_type),
      .extension_name = is_kernel ? "" : "ext_example",
      .scope = "instance",
      .user_id = {},
      .rbac_rule = "kernel.example",
      .enabled = enabled,
  };
}

} // namespace

TEST_CASE("Tier 1 hit dispatches to the registered handler",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:echo", "kernel.example", make_echo_handler("tier1-echo"));

  Json::Value args(Json::objectValue);
  args["hello"] = "world";
  auto out = plinth::capabilities::call_capability(
      CapabilityCall{
          .signature = "kernel:1:echo", .args = args, .call_depth = 0},
      default_ctx());

  REQUIRE(out.has_value());
  REQUIRE(out->resolved_tier == "tier1");
  REQUIRE(out->provider_type == "kernel");
  REQUIRE(out->data["tag"].asString() == "tier1-echo");
  REQUIRE(out->data["args"]["hello"].asString() == "world");
  // call_depth is incremented once at the dispatch boundary.
  REQUIRE(out->data["call_depth"].asInt() == 1);
}

TEST_CASE("Tier 2 miss returns capability_not_found",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "nosuch:1:anywhere"}, default_ctx());

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "capability_not_found");
}

TEST_CASE("Tier 2 hit with sidecar provider returns tier3_not_available",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("llm:1:complete", "sidecar"));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "llm:1:complete"}, default_ctx());

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "tier3_not_available");
}

TEST_CASE(
    "Tier 2 hit with extension provider on sync path yields async_required",
    "[capabilities][resolution][unit]") {
  // ICD-0.5.0.3 §Sync vs async — the sync `call_capability` entry
  // cannot drive JS evaluation, so every extension entry returns
  // `async_required`. JS + WS callers always go through
  // `call_capability_async`; this path is only hit by legacy in-kernel
  // callers that haven't migrated.
  fresh_resolver();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("terminal:1:shell", "extension"));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, default_ctx());

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "async_required");
}

TEST_CASE("Disabled capability is not dispatched",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("terminal:1:shell", "extension",
                          /*enabled=*/false));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, default_ctx());

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "capability_disabled");
}

TEST_CASE("User-scope entry overrides instance-scope for the calling user",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  // Instance-scope sidecar entry resolves to tier3_not_available.
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("llm:1:complete", "sidecar"));
  // User-scope entry for alice — disabled, so we can *observe* it was
  // the one selected. (If precedence were wrong, we'd get
  // tier3_not_available from the instance-scope row instead.)
  auto user_entry = make_instance_entry("llm:1:complete", "sidecar",
                                        /*enabled=*/false);
  user_entry.scope = "user";
  user_entry.user_id = "alice-uuid";
  plinth::capabilities::seed_tier2_cache_for_test(user_entry);

  auto ctx = default_ctx();
  ctx.user_id = "alice-uuid";

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "llm:1:complete"}, ctx);

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "capability_disabled");

  // And: a different user falls back to the instance-scope row.
  auto other = default_ctx();
  other.user_id = "other-uuid";
  auto out2 = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "llm:1:complete"}, other);
  REQUIRE_FALSE(out2.has_value());
  REQUIRE(plinth::capabilities::error_code(out2.error()) ==
          "tier3_not_available");
}

TEST_CASE("Call depth at the configured limit is rejected",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:deep", "kernel.example", make_echo_handler("deep"));

  // At exactly MAX_CALL_DEPTH we reject before dispatch.
  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "kernel:1:deep",
                     .call_depth = MAX_CALL_DEPTH},
      default_ctx());
  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "call_depth_exceeded");

  // MAX_CALL_DEPTH - 1 still dispatches.
  auto ok = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "kernel:1:deep",
                     .call_depth = MAX_CALL_DEPTH - 1},
      default_ctx());
  REQUIRE(ok.has_value());
  REQUIRE(ok->data["call_depth"].asInt() == MAX_CALL_DEPTH);
}

TEST_CASE("Malformed signatures return invalid_capability",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  for (const auto* bad :
       {"bad::thing", "nocolons", "a:b", "a:1:", ":1:fn", "a:-1:fn"}) {
    auto out = plinth::capabilities::call_capability(
        CapabilityCall{.signature = bad}, default_ctx());
    REQUIRE_FALSE(out.has_value());
    REQUIRE(plinth::capabilities::error_code(out.error()) ==
            "invalid_capability");
  }
}

TEST_CASE("upsert_tier2_entry inserts then replaces on the same key",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  auto e = make_instance_entry("fs:1:read", "extension");
  e.rbac_rule = "fs.read";
  plinth::capabilities::upsert_tier2_entry(e);
  REQUIRE(plinth::capabilities::tier2_cache_size() == 1);

  // Replace with a disabled variant of the same signature.
  e.enabled = false;
  plinth::capabilities::upsert_tier2_entry(e);
  REQUIRE(plinth::capabilities::tier2_cache_size() == 1);

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "fs:1:read"}, default_ctx());
  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "capability_disabled");
}

TEST_CASE("erase_tier2_entry removes the matching signature",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  plinth::capabilities::upsert_tier2_entry(
      make_instance_entry("fs:1:read", "extension"));
  REQUIRE(plinth::capabilities::tier2_cache_size() == 1);

  plinth::capabilities::erase_tier2_entry("fs:1:read", "instance");
  REQUIRE(plinth::capabilities::tier2_cache_size() == 0);

  // Erasing a missing entry is a no-op.
  plinth::capabilities::erase_tier2_entry("fs:1:read", "instance");
  REQUIRE(plinth::capabilities::tier2_cache_size() == 0);
}

TEST_CASE("set_enabled_by_extension_in_cache flips matching entries only",
          "[capabilities][resolution][unit]") {
  fresh_resolver();
  plinth::capabilities::upsert_tier2_entry(
      make_instance_entry("terminal:1:shell", "extension"));
  plinth::capabilities::upsert_tier2_entry(
      make_instance_entry("terminal:1:pty", "extension"));
  auto other = make_instance_entry("fs:1:read", "extension");
  other.extension_name = "ext_other";
  plinth::capabilities::upsert_tier2_entry(other);

  auto disabled = plinth::capabilities::set_enabled_by_extension_in_cache(
      "ext_example", false);
  REQUIRE(disabled == 2);

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, default_ctx());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "capability_disabled");
  auto out2 = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "fs:1:read"}, default_ctx());
  // ext_other was untouched — still enabled; sync path returns
  // async_required for extensions (ICD-0.5.0.3 §Sync vs async).
  REQUIRE(plinth::capabilities::error_code(out2.error()) == "async_required");

  auto enabled = plinth::capabilities::set_enabled_by_extension_in_cache(
      "ext_example", true);
  REQUIRE(enabled == 2);
  auto out3 = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, default_ctx());
  REQUIRE(plinth::capabilities::error_code(out3.error()) == "async_required");
}

TEST_CASE("Instance-scope Tier 2 dispatches when no user-scope entry exists",
          "[capabilities][resolution][unit]") {
  // Guards against accidentally making user-scope the *only* lookup
  // when ctx.user_id is non-empty (it must fall back).
  fresh_resolver();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("llm:1:complete", "sidecar"));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "llm:1:complete"}, default_ctx());

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "tier3_not_available");
}

// ── RBAC enforcement (ICD-0.2.4) ─────────────────────────────────────
//
// The cases below map 1:1 to ICD-0.2.4 §Milestone Criteria (Exit). Each
// exercises exactly one branch of the step-3 check so a regression in
// the permission algorithm or audit path localizes quickly.

TEST_CASE("RBAC: Tier 1 grant dispatches when user holds the rule",
          "[capabilities][resolution][rbac]") {
  fresh_resolver();
  plinth::capabilities::register_tier1_handler(
      "terminal:1:shell", "terminal.shell.execute",
      make_echo_handler("tier1-shell"));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"},
      ctx_with_rules({"terminal.shell.execute"}));

  REQUIRE(out.has_value());
  REQUIRE(out->resolved_tier == "tier1");
}

TEST_CASE("RBAC: Tier 2 grant passes the permission check",
          "[capabilities][resolution][rbac]") {
  // Extension providers on the sync path return async_required
  // (ICD-0.5.0.3 §Sync vs async). RBAC must still pass first — the
  // surfaced error is async_required, not permission_denied, which
  // proves the RBAC step succeeded before the sync-path rejection.
  fresh_resolver();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("fs:1:read", "extension"));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "fs:1:read"},
      ctx_with_rules({"kernel.example"}));

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "async_required");
}

TEST_CASE("RBAC: denial when user lacks the required rule",
          "[capabilities][resolution][rbac]") {
  fresh_resolver();
  plinth::capabilities::register_tier1_handler(
      "terminal:1:shell", "terminal.shell.execute",
      make_echo_handler("tier1-shell"));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"},
      ctx_with_rules({"fs.read", "some.other"}));

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "permission_denied");
}

TEST_CASE("RBAC: kernel.admin is a universal match",
          "[capabilities][resolution][rbac]") {
  fresh_resolver();
  plinth::capabilities::register_tier1_handler(
      "terminal:1:shell", "terminal.shell.execute",
      make_echo_handler("tier1-shell"));
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("fs:1:read", "extension"));

  auto admin = ctx_with_rules({"kernel.admin"});

  // Tier 1 hit dispatches despite admin not holding the concrete rule.
  auto t1 = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, admin);
  REQUIRE(t1.has_value());

  // Tier 2 RBAC passes; downstream async_required is the sync-path
  // dispatch-layer signal (ICD-0.5.0.3 §Sync vs async) — proves RBAC
  // did not deny.
  auto t2 = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "fs:1:read"}, admin);
  REQUIRE_FALSE(t2.has_value());
  REQUIRE(plinth::capabilities::error_code(t2.error()) == "async_required");
}

TEST_CASE("RBAC: per-hop check fails at the hop the user lacks",
          "[capabilities][resolution][rbac]") {
  // A Tier 1 handler that recursively invokes call_capability
  // simulates a multi-hop chain. The user holds A + B but not C;
  // A → B → C must deny at the C hop with permission_denied and
  // call_depth > 0.
  fresh_resolver();
  plinth::capabilities::register_tier1_handler("cap:1:c", "ns_c.run",
                                               make_echo_handler("c"));

  plinth::capabilities::register_tier1_handler(
      "cap:1:b", "ns_b.run",
      [](const Json::Value& /*args*/, const UserContext& nested,
         int call_depth) -> HandlerOutcome {
        auto inner = plinth::capabilities::call_capability(
            CapabilityCall{.signature = "cap:1:c", .call_depth = call_depth},
            nested);
        if (!inner.has_value()) {
          return std::unexpected(inner.error());
        }
        return inner->data;
      });

  plinth::capabilities::register_tier1_handler(
      "cap:1:a", "ns_a.run",
      [](const Json::Value& /*args*/, const UserContext& nested,
         int call_depth) -> HandlerOutcome {
        auto inner = plinth::capabilities::call_capability(
            CapabilityCall{.signature = "cap:1:b", .call_depth = call_depth},
            nested);
        if (!inner.has_value()) {
          return std::unexpected(inner.error());
        }
        return inner->data;
      });

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "cap:1:a", .call_depth = 0},
      ctx_with_rules({"ns_a.run", "ns_b.run"}));

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "permission_denied");
}

TEST_CASE("RBAC: denial at call_depth > 0 still denies",
          "[capabilities][resolution][rbac]") {
  fresh_resolver();
  plinth::capabilities::register_tier1_handler("terminal:1:shell",
                                               "terminal.shell.execute",
                                               make_echo_handler("nested"));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell", .call_depth = 3},
      ctx_with_rules({"fs.read"}));

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "permission_denied");
}

TEST_CASE(
    "RBAC: disabled capability surfaces capability_disabled when permitted",
    "[capabilities][resolution][rbac]") {
  // ICD-0.2.4 step ordering: RBAC (step 3) precedes dispatch (step
  // 4/5), so permission_denied shadows capability_disabled when the
  // user lacks the rule. Granting the rule lets dispatch surface
  // capability_disabled — this case pins that ordering.
  fresh_resolver();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("fs:1:read", "extension", /*enabled=*/false));

  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "fs:1:read"},
      ctx_with_rules({"kernel.example"}));

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "capability_disabled");
}

TEST_CASE("RBAC: fail-closed on cache entry with empty rbac_rule",
          "[capabilities][resolution][rbac]") {
  fresh_resolver();
  auto entry = make_instance_entry("fs:1:read", "extension");
  entry.rbac_rule.clear();
  plinth::capabilities::seed_tier2_cache_for_test(entry);

  // Even kernel.admin is denied when the entry carries no rule: the
  // check short-circuits on required_rule.empty() before the admin
  // match runs. This protects against misconfigured extensions.
  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "fs:1:read"},
      ctx_with_rules({"kernel.admin"}));

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "permission_denied");
}

TEST_CASE("RBAC: rule mapping is non-version-aware",
          "[capabilities][resolution][rbac]") {
  // Per ICD-0.2.4: v1 and v2 of the same capability share one rule.
  // A single grant of terminal.shell.execute lets the user call both.
  fresh_resolver();
  plinth::capabilities::register_tier1_handler(
      "terminal:1:shell", "terminal.shell.execute", make_echo_handler("v1"));
  plinth::capabilities::register_tier1_handler(
      "terminal:2:shell", "terminal.shell.execute", make_echo_handler("v2"));

  auto user = ctx_with_rules({"terminal.shell.execute"});

  auto v1 = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, user);
  auto v2 = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "terminal:2:shell"}, user);

  REQUIRE(v1.has_value());
  REQUIRE(v2.has_value());
  REQUIRE(v1->data["tag"].asString() == "v1");
  REQUIRE(v2->data["tag"].asString() == "v2");
}

TEST_CASE("RBAC: capability_not_found precedes permission_denied on cache miss",
          "[capabilities][resolution][rbac]") {
  // Calling a signature that is in neither map must not leak a
  // permission_denied response — we have no rule to report. This
  // pins the 0.2.2 precedent through the 0.2.4 change.
  fresh_resolver();
  auto out = plinth::capabilities::call_capability(
      CapabilityCall{.signature = "nowhere:1:fn"},
      ctx_with_rules({})); // no rules at all

  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) ==
          "capability_not_found");
}

// ── ICD-0.2.6 async wrapper parity ───────────────────────────────────
//
// Two cases per ICD-0.2.6 §Tests:
//   (1) Sync-async parity — five sub-assertions (Tier 1 hit, Tier 2 hit,
//       RBAC deny, capability-not-found, call-depth-exceeded). For each
//       fixture we call call_capability and call_capability_async via
//       drogon::sync_wait, then assert byte-identical ResolveResult.
//   (2) Coroutine-driver smoke — three sequential co_awaits inside a
//       single drogon::Task<void> harness, run via sync_wait. Proves the
//       wrapper composes in a coroutine context.
//
// The wrapper has no suspension point in 0.2.6 (the body is `co_return
// call_capability(call, ctx)`), so drogon::sync_wait spinning a worker
// thread is sufficient — no Drogon event loop is required for these
// cases.

TEST_CASE("call_capability_async matches sync result in every dispatch path",
          "[capabilities][resolution][async][unit]") {
  fresh_resolver();

  // Fixture: kernel:1:echo at Tier 1 with rule kernel.example.
  plinth::capabilities::register_tier1_handler(
      "kernel:1:echo", "kernel.example", make_echo_handler("parity-tier1"));

  // Fixture: kernel:1:cached at Tier 2 (kernel provider, instance scope).
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("kernel:1:cached", "kernel"));

  auto ctx_admin = ctx_with_rules({"kernel.admin"});
  auto ctx_no_rules = ctx_with_rules({});

  auto compare = [](const ResolveResult& sync_r, const ResolveResult& async_r) {
    REQUIRE(sync_r.has_value() == async_r.has_value());
    if (sync_r.has_value()) {
      REQUIRE(sync_r->data == async_r->data);
      REQUIRE(sync_r->resolved_tier == async_r->resolved_tier);
      REQUIRE(sync_r->provider_type == async_r->provider_type);
    } else {
      REQUIRE(plinth::capabilities::error_code(sync_r.error()) ==
              plinth::capabilities::error_code(async_r.error()));
    }
  };

  SECTION("Tier 1 hit") {
    Json::Value args(Json::objectValue);
    args["x"] = 1;
    CapabilityCall call{.signature = "kernel:1:echo", .args = args};
    auto sync_r = plinth::capabilities::call_capability(call, ctx_admin);
    auto async_r = drogon::sync_wait(
        plinth::capabilities::call_capability_async(call, ctx_admin));
    compare(sync_r, async_r);
  }

  SECTION("Tier 2 hit") {
    CapabilityCall call{.signature = "kernel:1:cached"};
    auto sync_r = plinth::capabilities::call_capability(call, ctx_admin);
    auto async_r = drogon::sync_wait(
        plinth::capabilities::call_capability_async(call, ctx_admin));
    compare(sync_r, async_r);
  }

  SECTION("RBAC deny") {
    CapabilityCall call{.signature = "kernel:1:echo"};
    auto sync_r = plinth::capabilities::call_capability(call, ctx_no_rules);
    auto async_r = drogon::sync_wait(
        plinth::capabilities::call_capability_async(call, ctx_no_rules));
    compare(sync_r, async_r);
    REQUIRE_FALSE(sync_r.has_value());
    REQUIRE(plinth::capabilities::error_code(sync_r.error()) ==
            "permission_denied");
  }

  SECTION("Capability not found") {
    CapabilityCall call{.signature = "nowhere:1:fn"};
    auto sync_r = plinth::capabilities::call_capability(call, ctx_admin);
    auto async_r = drogon::sync_wait(
        plinth::capabilities::call_capability_async(call, ctx_admin));
    compare(sync_r, async_r);
    REQUIRE_FALSE(sync_r.has_value());
    REQUIRE(plinth::capabilities::error_code(sync_r.error()) ==
            "capability_not_found");
  }

  SECTION("Call depth exceeded") {
    CapabilityCall call{.signature = "kernel:1:echo",
                        .call_depth = MAX_CALL_DEPTH};
    auto sync_r = plinth::capabilities::call_capability(call, ctx_admin);
    auto async_r = drogon::sync_wait(
        plinth::capabilities::call_capability_async(call, ctx_admin));
    compare(sync_r, async_r);
    REQUIRE_FALSE(sync_r.has_value());
    REQUIRE(plinth::capabilities::error_code(sync_r.error()) ==
            "call_depth_exceeded");
  }
}

TEST_CASE("call_capability_async composes inside a coroutine context",
          "[capabilities][resolution][async][unit]") {
  fresh_resolver();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:alpha", "kernel.example", make_echo_handler("A"));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:beta", "kernel.example", make_echo_handler("B"));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:gamma", "kernel.example", make_echo_handler("C"));

  auto ctx = ctx_with_rules({"kernel.admin"});

  // Three sequential co_awaits in a single drogon::Task<void>. The
  // task captures results via reference and the outer scope inspects
  // them after sync_wait returns.
  ResolveResult r1{std::unexpect, CapabilityError::CAPABILITY_NOT_FOUND};
  ResolveResult r2{std::unexpect, CapabilityError::CAPABILITY_NOT_FOUND};
  ResolveResult r3{std::unexpect, CapabilityError::CAPABILITY_NOT_FOUND};

  // sync_wait drives the task to completion before `driver` goes out
  // of scope, so the by-ref captures are safe for the full coroutine
  // lifetime.
  auto driver = [&]() -> drogon::Task<> {
    r1 = co_await plinth::capabilities::call_capability_async(
        CapabilityCall{.signature = "kernel:1:alpha"}, ctx);
    r2 = co_await plinth::capabilities::call_capability_async(
        CapabilityCall{.signature = "kernel:1:beta"}, ctx);
    r3 = co_await plinth::capabilities::call_capability_async(
        CapabilityCall{.signature = "kernel:1:gamma"}, ctx);
    co_return;
  };
  drogon::sync_wait(driver());

  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(r3.has_value());
  REQUIRE(r1->data["tag"].asString() == "A");
  REQUIRE(r2->data["tag"].asString() == "B");
  REQUIRE(r3->data["tag"].asString() == "C");
}
