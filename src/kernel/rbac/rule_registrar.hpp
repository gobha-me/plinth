#pragma once

// plinth::rbac::upsert_extension_rule — install-time RBAC rule insert.
//
// ICD-0.4.4 REGISTERING stage calls this once per entry in a package's
// `rbac.json`. Pure SQL helper — takes a borrowed `PGconn&` so the
// installer can wrap the entire REGISTERING stage in one transaction
// (UPDATE packages state → register_capability_tx × N →
// upsert_extension_rule × N → register_panel × N → UPDATE state →
// COMMIT). On ROLLBACK every rule insert is unwound.
//
// **No audit emission here.** The terminal `packages.installed` /
// `packages.install_failed` audit events carry the list of rules in
// their detail JSON, so per-rule audit would duplicate the record and
// risk orphan rows if the installer's outer transaction rolls back.
// Bootstrap-time kernel rule seeding (`bootstrap_groups`) audits
// separately because it runs pre-Drogon, one-shot, outside any
// transaction.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::rbac {

// UPSERT one RBAC rule into plinth.rbac_rules inside the caller's
// transaction. `ON CONFLICT (rule) DO UPDATE` refreshes the
// description / namespace / extension_name / test_contract so a
// reinstall picks up edits without needing a prior DELETE. Returns the
// empty value on success, an error string on PG failure.
//
// `test_contract` carries the rbac.json rule's `test` object verbatim
// (0.4.6 rule validator stores; 0.4.7 RBAC test runner reads). `std::nullopt`
// writes SQL NULL into the column. The parameter is additive and not defaulted
// per project convention.
//
// Preconditions:
//   - `conn` has `PQstatus == CONNECTION_OK`.
//   - Caller has already issued `BEGIN` if transactional rollback is
//     desired.
auto upsert_extension_rule(PGconn& conn, std::string_view rule,
                           std::string_view namespace_,
                           std::string_view description,
                           std::string_view extension_name,
                           std::optional<nlohmann::json> test_contract)
    -> std::expected<void, std::string>;

// 0.4.5 disable: set `orphaned_at = NOW()` on every rbac_rules row
// whose `extension_name` matches and `orphaned_at IS NULL`. Idempotent
// (already-orphaned rows untouched). Returns rows-affected count.
// `plinth.group_rules` grants are NOT touched — they survive re-enable
// per ICD-0.1.5 §Rule Lifecycle. Caller owns transaction.
auto mark_extension_rules_orphaned(std::string_view extension_name,
                                   PGconn& conn)
    -> std::expected<std::size_t, std::string>;

// 0.4.5 enable: set `orphaned_at = NULL` on every rbac_rules row whose
// `extension_name` matches. Idempotent. Returns rows-affected count.
auto clear_extension_rules_orphaned(std::string_view extension_name,
                                    PGconn& conn)
    -> std::expected<std::size_t, std::string>;

// 0.4.5 uninstall: DELETE every rbac_rules row for the extension.
// Caller is expected to have already stripped referencing group_rules
// rows (foreign-key violation otherwise). Returns rows-deleted count.
auto delete_extension_rules(std::string_view extension_name, PGconn& conn)
    -> std::expected<std::size_t, std::string>;

// 0.4.7 RBAC test: load every rbac_rules row for the extension,
// including rows with NULL `test_contract`. The skip-vs-run decision
// happens in the RBAC test runner, not in this helper — a NULL
// `test_contract` is reported in `RbacTestReport.skipped`, not dropped.
struct RbacTestRule {
  std::string rule;
  // keyword; trailing underscore matches project convention.
  std::string namespace_;
  std::string extension_name;
  std::optional<nlohmann::json> test_contract; // std::nullopt for SQL NULL
};

auto fetch_extension_rules_for_rbac_test(std::string_view extension_name,
                                         PGconn& conn)
    -> std::expected<std::vector<RbacTestRule>, std::string>;

} // namespace plinth::rbac
