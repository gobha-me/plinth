#pragma once

// plinth::capabilities — registration API.
//
// The four mutation functions defined by ICD-0.2.0 §Registration API.
// Every call:
//   1. Validates the input (validate_registration or a signature/scope pair).
//   2. Confirms the referenced RBAC rule exists (application-level check
//      per ICD §Validation Rules "not a database foreign key").
//   3. Executes the INSERT / UPDATE / DELETE against plinth.capabilities
//      inside a single transaction.
//   4. Emits the canonical audit event via log::audit_sync.
//   5. Emits a NOTIFY plinth_capability_changed with the JSON payload
//      defined by ICD-0.2.2-capability-resolution §Cache Invalidation.
//
// All functions are synchronous and libpq-based. The 0.4.x package
// install code will call them from inside the Drogon request handler
// that processes an install; registration is a rare operation and
// blocking the thread briefly is acceptable. An async Drogon wrapper
// can be added when and if the cost shows up in measurements.
//
// These are the same pattern as log::audit_sync (canonical sync path
// that opens its own short-lived libpq connection) and
// groups::bootstrap_groups (sync libpq mutations).

#include "kernel/capabilities/types.hpp"
#include "kernel/config.hpp"
#include "kernel/logging.hpp"

#include <expected>
#include <string>
#include <string_view>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::capabilities {

// Outcome of a registration / deregistration / enable / disable call.
//
// On success carries the canonical `namespace:version:function`
// signature (register_capability) or the echoed signature /
// extension_name that was acted on (deregister / disable / enable).
// On failure carries a CapabilityError code — see ICD-0.2.0
// §Standardized Error Shape and the CapabilityError enum in types.hpp.
//
// Defined here (not in types.hpp) so parser / validation / the fuzz
// harness don't transitively include `<expected>`; they only need
// `CapabilityError`.
using RegisterResult = std::expected<std::string, CapabilityError>;

// Insert one capability. On success, RegisterResult.result holds the
// canonical signature "namespace:version:function". On duplicate,
// returns capability_exists. On missing RBAC rule, rbac_rule_not_found.
auto register_capability(const Config::Database& db_cfg,
                         const CapabilityRegistration& reg,
                         const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult;

// Transactional variant: accepts a borrowed connection and runs the
// INSERT + `pg_notify` inside the caller's transaction (PG buffers
// NOTIFY until COMMIT, so a ROLLBACK correctly suppresses it). The
// caller owns BEGIN / COMMIT / ROLLBACK bracketing.
//
// **No audit emission.** ICD-0.4.4 §REGISTERING is the sole caller;
// terminal `packages.installed` / `packages.install_failed` audit
// events cover the whole install. Emitting per-capability audit here
// would risk orphan audit rows on ROLLBACK because `audit_sync` uses
// its own short-lived libpq connection outside any transaction.
//
// Consumers: 0.4.4 install_lifecycle REGISTERING. Other call sites
// should use the existing `register_capability(Config::Database, ...)`
// overload.
auto register_capability_tx(PGconn& conn, const CapabilityRegistration& reg)
    -> RegisterResult;

// Delete one capability by (signature, scope). Returns capability_not_found
// if no row matches.
auto deregister_capability(const Config::Database& db_cfg,
                           std::string_view signature, std::string_view scope,
                           const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult;

// 0.4.5 transactional deregister. DELETEs every row matching the
// (namespace, version, function) triple (both instance- and user-scope
// entries are removed — disable/uninstall want the whole capability
// gone). One `pg_notify(plinth_capability_changed, …)` per deleted
// scope so cache listeners can erase their Tier 2 entries precisely.
// Caller owns BEGIN/COMMIT bracketing; ROLLBACK suppresses the NOTIFY
// payload per PG semantics. Idempotent — missing rows are not an error.
//
// **No audit emission.** Terminal transition audit events
// (`packages.disabled` / `packages.uninstalled` / `packages.upgrade_*`)
// cover the capability changes as part of the transition record.
// Named `unregister_capability` per ICD-0.4.5 §Library Surface; kept in
// registration.hpp for symmetry with `register_capability_tx` rather
// than the ICD's resolution.hpp placement (it is a DB mutation, not a
// dispatch-cache concern).
auto unregister_capability(std::string_view ns, int version,
                           std::string_view function, PGconn& conn)
    -> std::expected<void, std::string>;

// Set enabled=false on every capability whose extension_name matches.
// RegisterResult.result holds the echoed extension_name on success.
auto disable_by_extension(const Config::Database& db_cfg,
                          std::string_view extension_name,
                          const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult;

// Set enabled=true on every capability whose extension_name matches.
auto enable_by_extension(const Config::Database& db_cfg,
                         std::string_view extension_name,
                         const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult;

} // namespace plinth::capabilities
