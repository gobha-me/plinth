#pragma once

// plinth::packages — structured migration-execution error type.
//
// 0.4.3 library surface for ICD-0.4.3. `MigrationFailure` is the
// `std::expected<_, E>` error arm of `run_migrations` /
// `drop_schema_and_migrations`. The shape mirrors
// `ManifestParseError` (file + kind + message + domain fields) but
// carries a PG `sqlstate` instead of a source-line pointer because
// migrations fail at the PG-exec layer.

#include <cstdint>
#include <optional>
#include <string>

namespace plinth::packages {

enum class MigrationError : std::uint8_t {
  DUPLICATE_SEQUENCE,
  INVALID_FILENAME,
  CHECKSUM_MISMATCH,
  SCHEMA_CREATE_FAILED,
  MIGRATION_APPLY_FAILED,
  DB_CONNECTION_BAD,
  READ_FAILED,
  ADVISORY_LOCK_FAILED,
  DROP_FAILED,
  UNQUALIFIED_DDL,
};

struct MigrationFailure {
  MigrationError kind;
  std::string extension_name;
  std::optional<std::string> migration_file;
  std::optional<std::string> pg_sqlstate;
  std::string message;
};

struct MigrationWarning {
  std::string kind;
  std::string detail;
};

} // namespace plinth::packages
