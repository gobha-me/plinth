#pragma once

#include <drogon/HttpFilter.h>
#include <optional>
#include <string>
#include <vector>

namespace plinth::rbac {

// ── Request attribute keys set by the RBAC enforcement filter ────────

inline constexpr auto ATTR_EFFECTIVE_RULES = "plinth.effective_rules";
inline constexpr auto ATTR_PERMISSION_GRANTED = "plinth.permission_granted";
inline constexpr auto ATTR_GRANTING_RULE = "plinth.granting_rule";

// ── RBAC context (populated after RbacFilter runs) ───────────────────

struct RbacContext {
  std::vector<std::string> effective_rules;
  bool permission_granted{false};
  std::string granting_rule; // empty if denied
};

// Extract the RbacContext from a request that has passed through RbacFilter.
// Returns nullopt if the filter has not run.
auto get_rbac_context(const drogon::HttpRequestPtr& req)
    -> std::optional<RbacContext>;

// ── Route-to-rules registry ──────────────────────────────────────────
//
// Populated at startup before app().run(); read-only at runtime.
// Maps (method, path_pattern) → required rules for a route.

struct RegisteredRule {
  drogon::HttpMethod method;
  std::string path_pattern;
  std::vector<std::string> rules;
};

auto register_rule_requirement(drogon::HttpMethod method,
                               const std::string& path_pattern,
                               std::vector<std::string> rules) -> void;

auto get_required_rules(drogon::HttpMethod method,
                        const std::string& path_pattern)
    -> std::optional<std::vector<std::string>>;

// Snapshot every (method, path_pattern, rules) triple currently in the
// registry, in registration order. Introduced for 0.2.6.1 so the
// anonymous-identity enforcement test can mechanically assert that
// every RBAC-gated route rejects `UserContext::anonymous()` — see
// architecture/01-identity.md §3 and tests/kernel/rbac/
// anonymous_identity_test.cpp. Returning a snapshot (rather than a
// view) insulates the test from future concurrency changes.
auto list_registered_rules() -> std::vector<RegisteredRule>;

// ── Drogon RBAC enforcement filter ───────────────────────────────────
//
// Must run AFTER plinth::auth::SessionFilter in the filter chain.
// Reads user_id from auth attributes, looks up required rules for the
// matched route, queries the user's effective rules (union across all
// groups), and either passes the request through or returns 403.
//
// On denial: returns 403 JSON, audits via plinth.audit_log.
// On grant: sets RBAC context attributes on the request.
// Fail-closed: any error during evaluation results in denial.

class RbacFilter : public drogon::HttpFilter<RbacFilter, false> {
 public:
  auto doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                drogon::FilterChainCallback&& fccb) -> void override;
};

} // namespace plinth::rbac
