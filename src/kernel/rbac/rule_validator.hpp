#pragma once

// plinth::rbac::validate_rules — ICD-0.4.6 install-time RBAC
// structural validation. Runs Rules A.1–A.5 against an already-parsed
// RbacManifest:
//   A.1  rule name matches ^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$
//   A.2  rule namespace equals the package name or a reserved kernel
//        namespace ({kernel, plinth, system})
//   A.3  each test.assert_*.call parses via parse_signature (ICD-0.2.1)
//   A.4  each test.assert_*.call resolves to a declared capability in
//        the sibling CapabilityManifest.provides[]
//   A.5  rule name is unique across extensions (SELECT against
//        plinth.rbac_rules; same-extension re-register is silent)
//
// All findings are reported as ManifestParseError with Severity::ERROR
// and rbac.* rule codes. The installer iterates the returned vector;
// any ERROR triggers the REGISTERING stage's RollbackGuard.
//
// Rule A.5 is the one database-coupled check; the caller owns the
// transaction (validate_rules is invoked inside the REGISTERING
// stage's BEGIN/COMMIT).

#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/manifest_error.hpp"
#include "kernel/rbac/rbac_manifest.hpp"

#include <string_view>
#include <vector>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::rbac {

auto validate_rules(const RbacManifest& rbac,
                    const plinth::packages::CapabilityManifest& caps,
                    std::string_view package_name, PGconn& conn)
    -> std::vector<plinth::packages::ManifestParseError>;

} // namespace plinth::rbac
