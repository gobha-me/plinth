#pragma once

// plinth::packages::detail — migrations internal helpers exposed for
// unit testing. Not part of the 0.4.3 public library surface. Only
// pure-logic helpers live here; PG-touching internals stay in the
// translation unit's anonymous namespace.

#include "migration_error.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages::detail {

struct MigrationFile {
  std::string filename;
  std::uint64_t sequence;
  std::string contents;
  std::string checksum_hex;
};

struct DiscoveryResult {
  std::vector<MigrationFile> migrations;
  std::vector<MigrationWarning> warnings;
};

// Returns the leading integer if `name` matches ^(\d+)_[a-z0-9_-]+\.sql$.
// Non-digit start, leading zero, missing underscore, empty tail, non-.sql
// extension, and bad-char-in-tail all return nullopt.
auto parse_migration_filename(std::string_view name)
    -> std::optional<std::uint64_t>;

// Strip a single leading UTF-8 BOM (EF BB BF) if present. No-op otherwise.
auto strip_utf8_bom(std::string& s) -> void;

// SHA-256 of the exact bytes passed, hex-encoded lowercase.
auto sha256_hex(std::string_view bytes) -> std::string;

// Scan `{migrations_dir}`. On a packaging bug (duplicate sequence,
// invalid filename, unqualified DDL), returns the failure. Missing
// directory is NOT an error here — the caller short-circuits before
// invoking this helper. Returns sorted-by-sequence MigrationFile
// vector plus any non-fatal warnings (e.g. sequence gaps).
auto discover_migrations(const std::filesystem::path& migrations_dir,
                         std::string_view extension_name)
    -> std::expected<DiscoveryResult, MigrationFailure>;

// Replace SQL line comments (`-- ... \n`), block comments (`/* ... */`,
// nestable per PG), single-quoted string literals (with `''` escapes),
// and dollar-quoted strings (`$tag$ ... $tag$`) with whitespace.
// Newlines are preserved so subsequent scanners can report line
// numbers. Used as preprocessing for `check_qualified_ddl` so a
// `CREATE TABLE` token inside a comment or literal is not flagged.
auto strip_sql_noise(std::string_view sql) -> std::string;

// Verify that every CREATE TABLE / VIEW / MATERIALIZED VIEW /
// SEQUENCE / TYPE / FUNCTION / PROCEDURE in `sql` qualifies its target
// name with `ext_{extension_name}.`. Returns nullopt on success;
// otherwise a human-readable explanation of the first violation.
//
// Migration SQL must use fully-qualified `ext_{name}.<obj>` per
// ICD-0.4.3 §Schema + GRANT Contract — `search_path` is deliberately
// NOT set during apply, so unqualified DDL would land in the admin
// connection's default schema rather than `ext_{name}`.
auto check_qualified_ddl(std::string_view sql, std::string_view extension_name)
    -> std::optional<std::string>;

} // namespace plinth::packages::detail
