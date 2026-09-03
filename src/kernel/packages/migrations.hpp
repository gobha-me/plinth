#pragma once

// plinth::packages::migrations — extension PG schema + migration library.
//
// ICD-0.4.3. 0.4.3 is library-only: no CLI, no HTTP surface. The sole
// in-tree caller is 0.4.4's install lifecycle (MIGRATING stage).
// Connection lifetime is the caller's responsibility — this module
// does not `PQfinish` the `PGconn&` it receives.

#include "migration_error.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::packages {

struct MigrationReport {
  std::vector<std::string> applied;
  std::vector<std::string> skipped;
  std::vector<MigrationWarning> warnings;
};

// Idempotent. Creates ext_{extension_name} schema + ext_{extension_name}_role
// + GRANTs on the first call, then applies unseen migrations from
// {package_root}/migrations in numeric order, recording each in
// plinth.migrations with a SHA-256 checksum. Re-running with the same
// package is a no-op that returns every migration in `skipped`.
//
// Preconditions:
//   - `extension_name` matches ^[a-z][a-z0-9_-]{2,62}$ (0.4.1 regex).
//   - `admin_conn` has `PQstatus == CONNECTION_OK` and CREATE privileges.
auto run_migrations(std::string_view extension_name,
                    const std::filesystem::path& package_root,
                    PGconn& admin_conn)
    -> std::expected<MigrationReport, MigrationFailure>;

// Companion teardown for 0.4.4's first-install failure path. Drops
// ext_{extension_name} CASCADE, drops ext_{extension_name}_role (if
// present), and deletes the extension's rows from plinth.migrations.
// Idempotent: missing schema / role / rows are not errors.
auto drop_schema_and_migrations(std::string_view extension_name,
                                PGconn& admin_conn)
    -> std::expected<void, MigrationFailure>;

} // namespace plinth::packages
