#pragma once

// plinth::rbac — ephemeral RBAC-test user factory (ICD-0.4.7).
//
// The RBAC test runner runs every rule's `assert_deny` / `assert_allow`
// contract as a synthesised `UserContext` that never authenticates over HTTP.
// One run creates two users:
//   __test_denied_<run_id>   — `everyone` only; expected to be denied.
//   __test_allowed_<run_id>  — `everyone` + synthetic `__rbac_test_<run_id>`
//                              group that receives the single rule under
//                              test (grant/revoke cycles between rules).
//
// Both users carry `is_test_user = true` + a placeholder argon2id hash
// that verifies against no password — the admin user-listing queries
// filter on `is_test_user = false`, and the HTTP login path cannot
// succeed against these rows even if the placeholder were known.
//
// `destroy_run_users` is idempotent; it runs on both the happy-path
// end of `run_rbac_test` and the failure path. The reconciler's
// `cleanup_orphaned_test_users` sweep is the last line of defence
// against process-crash mid-run leaks.

#include "kernel/capabilities/resolution.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::rbac {

struct RunUserPair {
  std::string run_id;
  std::string denied_user_id;
  std::string denied_username;
  std::string allowed_user_id;
  std::string allowed_username;
  std::string allowed_group_id; // __rbac_test_<run_id>
  std::string allowed_group_name;
};

// Creates the denied user, the allowed user, and the synthetic group,
// and issues `everyone` membership for both users plus synthetic-group
// membership for the allowed user. Preconditions: caller has acquired
// the per-name advisory lock; caller owns the transaction. Returns the
// pair on success or a PG error message on failure. On failure the
// caller MUST call `destroy_run_users` to back out any partial inserts.
auto create_run_users(std::string_view run_id, PGconn& conn)
    -> std::expected<RunUserPair, std::string>;

// Issue the single-rule grant on the synthetic group. Called at the
// top of every rule iteration; `revoke_rule_from_run_group` mirrors it
// at the bottom so the allowed user has `effective_rules == {}`
// between rules. Both operations are idempotent at the group_rules
// level (INSERT…ON CONFLICT DO NOTHING / unconditional DELETE).
auto grant_rule_to_run_group(std::string_view run_id, std::string_view rule,
                             PGconn& conn) -> std::expected<void, std::string>;
auto revoke_rule_from_run_group(std::string_view run_id, std::string_view rule,
                                PGconn& conn)
    -> std::expected<void, std::string>;

// Idempotent teardown in dependency order: group_rules → group_members
// → users → groups. Safe to call on a run that got halfway or all the
// way; missing rows are no-ops.
auto destroy_run_users(std::string_view run_id, PGconn& conn)
    -> std::expected<void, std::string>;

// Reconciler sweep. For every user with `is_test_user = true` AND
// `created_at < older_than`, destroy it and its associated
// `__rbac_test_<run_id>` group. Returns the number of runs cleaned up
// (i.e., distinct run_ids, derived from the username suffix).
auto cleanup_orphaned_test_users(
    std::chrono::system_clock::time_point older_than, PGconn& conn)
    -> std::expected<std::size_t, std::string>;

// Synthesise a `UserContext` for RBAC-test dispatch — `auth_type = "test"`
// so audit rows emitted inside the capability call are trivially
// distinguishable from real-user audit rows. `effective_rules` is
// computed from the synthetic group's current grants at call time (the
// single rule under test for `assert_allow`; empty for `assert_deny`).
auto build_test_user_context(std::string_view username,
                             std::string_view user_id,
                             std::vector<std::string> effective_rules)
    -> plinth::capabilities::UserContext;

} // namespace plinth::rbac
