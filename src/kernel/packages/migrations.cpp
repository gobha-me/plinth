#include "kernel/packages/migrations.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/packages/migrations_internal.hpp"

#include <libpq-fe.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace plinth::packages {

namespace detail {

auto parse_migration_filename(std::string_view name)
    -> std::optional<std::uint64_t> {
  // ^(\d+)_[a-z0-9_-]+\.sql$
  static const std::regex PATTERN{R"(^(\d+)_[a-z0-9_-]+\.sql$)"};
  std::string sname{name};
  std::smatch m;
  if (!std::regex_match(sname, m, PATTERN)) {
    return std::nullopt;
  }
  std::uint64_t seq = 0;
  for (char ch : m[1].str()) {
    seq = (seq * 10U) + static_cast<std::uint64_t>(ch - '0');
  }
  return seq;
}

auto strip_utf8_bom(std::string& s) -> void {
  static constexpr std::array<unsigned char, 3> BOM{0xEFU, 0xBBU, 0xBFU};
  if (s.size() >= BOM.size() && static_cast<unsigned char>(s[0]) == BOM[0] &&
      static_cast<unsigned char>(s[1]) == BOM[1] &&
      static_cast<unsigned char>(s[2]) == BOM[2]) {
    s.erase(0, BOM.size());
  }
}

namespace {

auto blank_for(char c) -> char {
  return c == '\n' ? '\n' : ' ';
}

auto blank_range(std::string_view sql, std::size_t from, std::size_t to,
                 std::string& out) -> void {
  for (std::size_t k = from; k < to; ++k) {
    out.push_back(blank_for(sql[k]));
  }
}

auto skip_line_comment(std::string_view sql, std::size_t i, std::string& out)
    -> std::size_t {
  while (i < sql.size() && sql[i] != '\n') {
    out.push_back(blank_for(sql[i]));
    ++i;
  }
  return i;
}

auto skip_block_comment(std::string_view sql, std::size_t i, std::string& out)
    -> std::size_t {
  int depth = 1;
  blank_range(sql, i, i + 2, out);
  i += 2;
  while (i < sql.size() && depth > 0) {
    if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
      ++depth;
      blank_range(sql, i, i + 2, out);
      i += 2;
    } else if (i + 1 < sql.size() && sql[i] == '*' && sql[i + 1] == '/') {
      --depth;
      blank_range(sql, i, i + 2, out);
      i += 2;
    } else {
      out.push_back(blank_for(sql[i]));
      ++i;
    }
  }
  return i;
}

auto skip_single_quoted(std::string_view sql, std::size_t i, std::string& out)
    -> std::size_t {
  out.push_back(blank_for(sql[i]));
  ++i;
  while (i < sql.size()) {
    if (sql[i] != '\'') {
      out.push_back(blank_for(sql[i]));
      ++i;
      continue;
    }
    if (i + 1 < sql.size() && sql[i + 1] == '\'') {
      blank_range(sql, i, i + 2, out);
      i += 2;
      continue;
    }
    out.push_back(blank_for(sql[i]));
    ++i;
    return i;
  }
  return i;
}

auto find_dollar_tag_end(std::string_view sql, std::size_t i) -> std::size_t {
  std::size_t j = i + 1;
  while (j < sql.size() &&
         (std::isalnum(static_cast<unsigned char>(sql[j])) != 0 ||
          sql[j] == '_')) {
    ++j;
  }
  return j;
}

auto skip_dollar_quoted(std::string_view sql, std::size_t i,
                        std::string_view tag, std::string& out) -> std::size_t {
  blank_range(sql, i, i + tag.size(), out);
  i += tag.size();
  while (i + tag.size() <= sql.size() && sql.substr(i, tag.size()) != tag) {
    out.push_back(blank_for(sql[i]));
    ++i;
  }
  if (i + tag.size() <= sql.size()) {
    blank_range(sql, i, i + tag.size(), out);
    i += tag.size();
  }
  return i;
}

} // namespace

auto strip_sql_noise(std::string_view sql) -> std::string {
  std::string out;
  out.reserve(sql.size());
  std::size_t i = 0;
  while (i < sql.size()) {
    char c = sql[i];
    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      i = skip_line_comment(sql, i, out);
    } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      i = skip_block_comment(sql, i, out);
    } else if (c == '\'') {
      i = skip_single_quoted(sql, i, out);
    } else if (c == '$') {
      std::size_t j = find_dollar_tag_end(sql, i);
      if (j < sql.size() && sql[j] == '$') {
        i = skip_dollar_quoted(sql, i, sql.substr(i, (j - i) + 1), out);
      } else {
        out.push_back(c);
        ++i;
      }
    } else {
      out.push_back(c);
      ++i;
    }
  }
  return out;
}

namespace {

auto unquote_schema(std::string_view ident) -> std::string {
  if (ident.size() >= 2 && ident.front() == '"' && ident.back() == '"') {
    std::string inner{ident.substr(1, ident.size() - 2)};
    std::string out;
    out.reserve(inner.size());
    for (std::size_t k = 0; k < inner.size(); ++k) {
      if (inner[k] == '"' && k + 1 < inner.size() && inner[k + 1] == '"') {
        out.push_back('"');
        ++k;
      } else {
        out.push_back(inner[k]);
      }
    }
    return out;
  }
  return std::string{ident};
}

auto split_qualified_name(std::string_view name)
    -> std::pair<std::string_view, std::string_view> {
  bool in_quote = false;
  for (std::size_t k = 0; k < name.size(); ++k) {
    char c = name[k];
    if (c == '"') {
      in_quote = !in_quote;
    } else if (c == '.' && !in_quote) {
      return {name.substr(0, k), name.substr(k + 1)};
    }
  }
  return {std::string_view{}, name};
}

auto is_qualified_to_ext(std::string_view name, std::string_view extension_name)
    -> bool {
  auto schema = split_qualified_name(name).first;
  if (schema.empty()) {
    return false;
  }
  auto schema_text = unquote_schema(schema);
  std::string expected = "ext_";
  expected += extension_name;
  return schema_text == expected;
}

} // namespace

auto check_qualified_ddl(std::string_view sql, std::string_view extension_name)
    -> std::optional<std::string> {
  auto stripped = strip_sql_noise(sql);

  // CREATE [OR REPLACE] [GLOBAL|LOCAL] [TEMP|TEMPORARY|UNLOGGED|RECURSIVE]
  //   (TABLE|MATERIALIZED VIEW|VIEW|SEQUENCE|TYPE|FUNCTION|PROCEDURE)
  //   [IF NOT EXISTS] <ident>
  static const std::regex PATTERN{
      R"((?:^|[^A-Za-z0-9_]))"
      R"(CREATE\s+)"
      R"((?:OR\s+REPLACE\s+)?)"
      R"((?:GLOBAL\s+|LOCAL\s+)?)"
      R"((?:TEMP(?:ORARY)?\s+|UNLOGGED\s+|RECURSIVE\s+)?)"
      R"((TABLE|MATERIALIZED\s+VIEW|VIEW|SEQUENCE|TYPE|FUNCTION|PROCEDURE))"
      R"(\s+)"
      R"((?:IF\s+NOT\s+EXISTS\s+)?)"
      R"(([^\s(;,]+))",
      std::regex::icase};

  auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), PATTERN);
  auto end = std::sregex_iterator{};
  for (auto it = begin; it != end; ++it) {
    const auto& m = *it;
    std::string kind = m[1].str();
    std::string name = m[2].str();
    if (!is_qualified_to_ext(name, extension_name)) {
      std::ostringstream msg;
      msg << "unqualified DDL: `CREATE " << kind << " " << name
          << "` must be schema-qualified as `ext_" << extension_name
          << ".<name>` (per ICD-0.4.3 \xc2\xa7Schema + GRANT Contract"
             " \xe2\x80\x94 search_path is deliberately not set during"
             " migration apply, so unqualified DDL lands in the admin"
             " connection's default schema rather than ext_"
          << extension_name << ")";
      return msg.str();
    }
  }
  return std::nullopt;
}

auto sha256_hex(std::string_view bytes) -> std::string {
  auto* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    throw std::runtime_error("EVP_MD_CTX_new failed");
  }
  std::array<std::uint8_t, 32> hash{};
  unsigned int hash_len = 0;
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(ctx, hash.data(), &hash_len) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("SHA-256 computation failed");
  }
  EVP_MD_CTX_free(ctx);

  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < hash_len; ++i) {
    ss << std::setw(2) << static_cast<int>(hash.at(i));
  }
  return ss.str();
}

namespace {

auto looks_like_migration_candidate(std::string_view name) -> bool {
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) == 0) {
    return false;
  }
  return name.ends_with(".sql");
}

auto read_file(const fs::path& path, std::string& out, std::string& err_out)
    -> bool {
  std::ifstream in{path, std::ios::binary};
  if (!in.is_open()) {
    err_out = "cannot open file";
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (in.bad()) {
    err_out = "read error";
    return false;
  }
  out = buf.str();
  return true;
}

auto list_dir_entries(const fs::path& dir, std::string_view extension_name)
    -> std::expected<std::vector<fs::directory_entry>, MigrationFailure> {
  std::vector<fs::directory_entry> entries;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator{dir, ec}) {
    if (ec) {
      return std::unexpected(MigrationFailure{
          .kind = MigrationError::READ_FAILED,
          .extension_name = std::string{extension_name},
          .migration_file = std::nullopt,
          .pg_sqlstate = std::nullopt,
          .message = "failed to iterate migrations directory: " + ec.message(),
      });
    }
    entries.push_back(entry);
  }
  if (ec) {
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::READ_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = std::nullopt,
        .pg_sqlstate = std::nullopt,
        .message = "failed to open migrations directory: " + ec.message(),
    });
  }
  std::ranges::sort(
      entries, [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
      });
  return entries;
}

auto build_migration_file(const fs::directory_entry& entry,
                          std::string_view extension_name)
    -> std::expected<std::optional<MigrationFile>, MigrationFailure> {
  if (!entry.is_regular_file()) {
    return std::optional<MigrationFile>{};
  }
  auto filename = entry.path().filename().string();
  auto seq = parse_migration_filename(filename);
  if (!seq.has_value()) {
    if (!looks_like_migration_candidate(filename)) {
      return std::optional<MigrationFile>{};
    }
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::INVALID_FILENAME,
        .extension_name = std::string{extension_name},
        .migration_file = filename,
        .pg_sqlstate = std::nullopt,
        .message = "invalid migration filename: " + filename +
                   " (expected NNN_description.sql)",
    });
  }
  std::string contents;
  std::string err;
  if (!read_file(entry.path(), contents, err)) {
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::READ_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = filename,
        .pg_sqlstate = std::nullopt,
        .message = "failed to read migration file " + filename + ": " + err,
    });
  }
  strip_utf8_bom(contents);
  return std::optional<MigrationFile>{MigrationFile{
      .filename = filename,
      .sequence = *seq,
      .contents = contents,
      .checksum_hex = sha256_hex(contents),
  }};
}

auto detect_duplicate(const std::vector<MigrationFile>& sorted,
                      std::string_view extension_name)
    -> std::optional<MigrationFailure> {
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].sequence == sorted[i - 1].sequence) {
      return MigrationFailure{
          .kind = MigrationError::DUPLICATE_SEQUENCE,
          .extension_name = std::string{extension_name},
          .migration_file = sorted[i].filename,
          .pg_sqlstate = std::nullopt,
          .message = "duplicate migration sequence " +
                     std::to_string(sorted[i].sequence) + ": " +
                     sorted[i - 1].filename + ", " + sorted[i].filename,
      };
    }
  }
  return std::nullopt;
}

auto build_gap_warning(const std::vector<MigrationFile>& sorted)
    -> std::optional<MigrationWarning> {
  if (sorted.size() < 2) {
    return std::nullopt;
  }
  std::vector<std::uint64_t> missing;
  auto first_seq = sorted.front().sequence;
  auto last_seq = sorted.back().sequence;
  std::size_t cursor = 0;
  for (auto s = first_seq; s <= last_seq; ++s) {
    if (cursor < sorted.size() && sorted[cursor].sequence == s) {
      ++cursor;
    } else {
      missing.push_back(s);
    }
  }
  if (missing.empty()) {
    return std::nullopt;
  }
  std::ostringstream detail;
  detail << "migration sequence " << first_seq << " \xE2\x86\x92 " << last_seq
         << " skips numbers: ";
  for (std::size_t i = 0; i < missing.size(); ++i) {
    if (i > 0) {
      detail << ", ";
    }
    detail << missing[i];
  }
  return MigrationWarning{
      .kind = "migration-gap",
      .detail = detail.str(),
  };
}

} // namespace

auto discover_migrations(const fs::path& migrations_dir,
                         std::string_view extension_name)
    -> std::expected<DiscoveryResult, MigrationFailure> {
  auto entries = list_dir_entries(migrations_dir, extension_name);
  if (!entries.has_value()) {
    return std::unexpected(entries.error());
  }

  DiscoveryResult result;
  for (const auto& entry : *entries) {
    auto built = build_migration_file(entry, extension_name);
    if (!built.has_value()) {
      return std::unexpected(built.error());
    }
    if (built->has_value()) {
      result.migrations.push_back(std::move(**built));
    }
  }

  std::ranges::sort(result.migrations,
                    [](const MigrationFile& a, const MigrationFile& b) {
                      return a.sequence < b.sequence;
                    });

  if (auto dup = detect_duplicate(result.migrations, extension_name);
      dup.has_value()) {
    return std::unexpected(*dup);
  }

  for (const auto& m : result.migrations) {
    if (auto err = check_qualified_ddl(m.contents, extension_name);
        err.has_value()) {
      return std::unexpected(MigrationFailure{
          .kind = MigrationError::UNQUALIFIED_DDL,
          .extension_name = std::string{extension_name},
          .migration_file = m.filename,
          .pg_sqlstate = std::nullopt,
          .message = "migration " + m.filename + ": " + *err,
      });
    }
  }

  if (auto gap = build_gap_warning(result.migrations); gap.has_value()) {
    result.warnings.push_back(*gap);
  }

  return result;
}

} // namespace detail

namespace {

using plinth::packages::detail::discover_migrations;
using plinth::packages::detail::MigrationFile;
using plinth::packages::detail::sha256_hex;

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto make_result(PGresult* r) -> PgResultPtr {
  return {r, PQclear};
}

auto get_sqlstate(PGresult* res) -> std::optional<std::string> {
  if (res == nullptr) {
    return std::nullopt;
  }
  const char* state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
  if (state == nullptr || *state == '\0') {
    return std::nullopt;
  }
  return std::string{state};
}

auto exec_ok(PGconn& conn, const std::string& sql)
    -> std::optional<std::string> {
  auto res = make_result(plinth::db::exec(&conn, sql.c_str()));
  auto status = PQresultStatus(res.get());
  if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
    return std::nullopt;
  }
  const char* msg = PQresultErrorMessage(res.get());
  std::string err = (msg == nullptr) ? "" : msg;
  while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) {
    err.pop_back();
  }
  return err;
}

auto exec_with_params(PGconn& conn, const char* sql,
                      const std::vector<std::string>& params) -> PgResultPtr {
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& p : params) {
    values.push_back(p.c_str());
  }
  return make_result(
      plinth::db::exec_params(&conn, sql, static_cast<int>(params.size()),
                              nullptr, values.data(), nullptr, nullptr, 0));
}

auto schema_exists(PGconn& conn, std::string_view name)
    -> std::expected<bool, std::string> {
  auto res =
      exec_with_params(conn, "SELECT 1 FROM pg_namespace WHERE nspname = $1",
                       {"ext_" + std::string{name}});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    const char* msg = PQresultErrorMessage(res.get());
    return std::unexpected(msg == nullptr ? "" : msg);
  }
  return PQntuples(res.get()) > 0;
}

auto pq_escape(PGconn& conn, std::string_view s) -> std::string {
  auto* escaped = PQescapeLiteral(&conn, s.data(), s.size());
  if (escaped == nullptr) {
    throw std::runtime_error("PQescapeLiteral failed");
  }
  std::string out{escaped};
  PQfreemem(escaped);
  return out;
}

// Acquire an advisory lock keyed by hashtextextended('plinth.migrations.' ||
// name, 0). Returns true on acquire, false if another session holds it.
auto try_advisory_lock(PGconn& conn, std::string_view name, bool caller_owned)
    -> std::expected<bool, std::string> {
  std::string sql =
      std::string{caller_owned
                      ? "SELECT pg_try_advisory_xact_lock(hashtextextended("
                      : "SELECT pg_try_advisory_lock(hashtextextended("} +
      pq_escape(conn, std::string{"plinth.migrations."} + std::string{name}) +
      ", 0))";
  auto res = make_result(plinth::db::exec(&conn, sql.c_str()));
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) < 1) {
    const char* msg = PQresultErrorMessage(res.get());
    return std::unexpected(msg == nullptr ? "" : msg);
  }
  const char* v = PQgetvalue(res.get(), 0, 0);
  return (v != nullptr) && (*v == 't');
}

auto advisory_unlock(PGconn& conn, std::string_view name) -> void {
  std::string sql =
      "SELECT pg_advisory_unlock(hashtextextended(" +
      pq_escape(conn, std::string{"plinth.migrations."} + std::string{name}) +
      ", 0))";
  auto res = make_result(plinth::db::exec(&conn, sql.c_str()));
  (void)res;
}

class AdvisoryLock {
 public:
  AdvisoryLock(PGconn& conn, std::string_view name, bool held)
      : conn(conn), name(name), held(held) {}
  ~AdvisoryLock() {
    if (held) {
      advisory_unlock(conn, name);
    }
  }
  AdvisoryLock(const AdvisoryLock&) = delete;
  auto operator=(const AdvisoryLock&) -> AdvisoryLock& = delete;
  AdvisoryLock(AdvisoryLock&&) = delete;
  auto operator=(AdvisoryLock&&) -> AdvisoryLock& = delete;

 private:
  PGconn& conn;
  std::string name;
  bool held;
};

auto ensure_schema_and_role(PGconn& conn, std::string_view name,
                            bool manage_transaction)
    -> std::optional<MigrationFailure> {
  std::string ext = "ext_";
  ext += name;
  std::string role = ext + "_role";
  std::ostringstream sql;
  if (manage_transaction) {
    sql << "BEGIN;\n";
  }
  sql << "  CREATE SCHEMA IF NOT EXISTS " << ext << ";\n";
  sql << "  DO $$ BEGIN\n"
      << "    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = "
      << pq_escape(conn, role) << ") THEN\n"
      << "      CREATE ROLE " << role << " NOLOGIN;\n"
      << "    END IF;\n"
      << "  END $$;\n";
  sql << "  GRANT USAGE, CREATE ON SCHEMA " << ext << " TO " << role << ";\n";
  sql << "  GRANT USAGE ON SCHEMA plinth TO " << role << ";\n";
  sql << "  GRANT SELECT ON plinth.users TO " << role << ";\n";
  if (manage_transaction) {
    sql << "COMMIT;";
  }

  auto res = make_result(plinth::db::exec(&conn, sql.str().c_str()));
  auto status = PQresultStatus(res.get());
  if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
    return std::nullopt;
  }
  auto sqlstate = get_sqlstate(res.get());
  const char* msg = PQresultErrorMessage(res.get());
  std::string err = (msg == nullptr) ? "" : msg;
  while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) {
    err.pop_back();
  }
  if (manage_transaction) {
    (void)exec_ok(conn, "ROLLBACK");
  }
  return MigrationFailure{
      .kind = MigrationError::SCHEMA_CREATE_FAILED,
      .extension_name = std::string{name},
      .migration_file = std::nullopt,
      .pg_sqlstate = sqlstate,
      .message = "schema or role setup for " + ext + " failed: " + err,
  };
}

auto already_applied_checksum(PGconn& conn, std::string_view extension_name,
                              const std::string& filename)
    -> std::expected<std::optional<std::string>, MigrationFailure> {
  auto res =
      exec_with_params(conn,
                       "SELECT checksum FROM plinth.migrations "
                       "WHERE extension_name = $1 AND migration_file = $2",
                       {std::string{extension_name}, filename});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    const char* msg = PQresultErrorMessage(res.get());
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::MIGRATION_APPLY_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = filename,
        .pg_sqlstate = get_sqlstate(res.get()),
        .message =
            "checksum lookup failed: " + std::string{msg == nullptr ? "" : msg},
    });
  }
  if (PQntuples(res.get()) == 0) {
    return std::optional<std::string>{};
  }
  const char* v = PQgetvalue(res.get(), 0, 0);
  return std::optional<std::string>{v == nullptr ? "" : v};
}

struct MigrationStatement {
  std::size_t length = 0;
  bool transaction_control = false;
};

// Split only at a semicolon outside PostgreSQL literals/comments. Execution
// additionally uses the extended-query protocol, so a mistaken boundary can
// never smuggle a second command into a statement accepted by this lexer.
auto next_migration_statement(std::string_view sql, bool standard_strings)
    -> std::expected<MigrationStatement, std::string> {
  if (sql.find('\0') != std::string_view::npos) {
    return std::unexpected("migration SQL contains a NUL byte");
  }
  std::array<std::string, 2> words;
  std::size_t word_count = 0;
  std::size_t i = 0;
  auto identifier_start = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' ||
           static_cast<unsigned char>(c) >= 128;
  };
  auto identifier_part = [&](char c) {
    return identifier_start(c) ||
           std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '$';
  };
  while (i < sql.size()) {
    const char c = sql[i];
    if (c == ';') {
      ++i;
      break;
    }
    if (sql.substr(i, 2) == "--") {
      const auto end = sql.find('\n', i + 2);
      i = end == std::string_view::npos ? sql.size() : end;
      continue;
    }
    if (sql.substr(i, 2) == "/*") {
      std::size_t depth = 1;
      i += 2;
      while (i < sql.size() && depth != 0) {
        if (sql.substr(i, 2) == "/*") {
          ++depth;
          i += 2;
        } else if (sql.substr(i, 2) == "*/") {
          --depth;
          i += 2;
        } else {
          ++i;
        }
      }
      if (depth != 0) {
        return std::unexpected("unterminated migration SQL comment");
      }
      continue;
    }
    const bool escape_string =
        (c == 'E' || c == 'e') && sql.substr(i + 1, 1) == "'";
    if (c == '\'' || c == '"' || escape_string) {
      const char quote = escape_string ? '\'' : c;
      const bool backslash_escapes =
          quote == '\'' && (escape_string || !standard_strings);
      i += escape_string ? 2 : 1;
      bool closed = false;
      while (i < sql.size()) {
        if (sql[i] == '\\' && backslash_escapes) {
          i += std::min(std::size_t{2}, sql.size() - i);
        } else if (sql[i] == quote) {
          ++i;
          if (i < sql.size() && sql[i] == quote) {
            ++i;
          } else {
            closed = true;
            break;
          }
        } else {
          ++i;
        }
      }
      if (!closed) {
        return std::unexpected("unterminated migration SQL literal");
      }
      continue;
    }
    if (c == '$') {
      std::size_t end = i + 1;
      if (end < sql.size() && identifier_start(sql[end])) {
        while (end < sql.size() && sql[end] != '$' &&
               identifier_part(sql[end])) {
          ++end;
        }
      }
      if (end < sql.size() && sql[end] == '$') {
        const auto tag = sql.substr(i, end - i + 1);
        const auto close = sql.find(tag, end + 1);
        if (close == std::string_view::npos) {
          return std::unexpected("unterminated migration SQL dollar literal");
        }
        i = close + tag.size();
        continue;
      }
    }
    if (identifier_start(c)) {
      const auto start = i++;
      while (i < sql.size() && identifier_part(sql[i])) {
        ++i;
      }
      if (word_count < words.size()) {
        auto& word = words[word_count++];
        word = sql.substr(start, i - start);
        std::ranges::transform(word, word.begin(), [](unsigned char ch) {
          return static_cast<char>(std::toupper(ch));
        });
      }
      continue;
    }
    ++i;
  }
  const auto& command = words[0];
  return MigrationStatement{
      .length = i,
      .transaction_control =
          command == "BEGIN" || command == "START" || command == "COMMIT" ||
          command == "END" || command == "ROLLBACK" || command == "ABORT" ||
          (command == "PREPARE" && words[1] == "TRANSACTION")};
}

auto apply_one(PGconn& conn, std::string_view extension_name,
               const MigrationFile& file, bool manage_transaction)
    -> std::optional<MigrationFailure> {
  auto begin_error = manage_transaction ? exec_ok(conn, "BEGIN")
                                        : std::optional<std::string>{};
  if (const auto& err = begin_error; err.has_value()) {
    return MigrationFailure{
        .kind = MigrationError::MIGRATION_APPLY_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = file.filename,
        .pg_sqlstate = std::nullopt,
        .message = "BEGIN failed: " + *err,
    };
  }

  std::size_t offset = 0;
  do {
    std::string statement;
    if (!manage_transaction) {
      const char* strings =
          PQparameterStatus(&conn, "standard_conforming_strings");
      auto parsed = next_migration_statement(
          std::string_view{file.contents}.substr(offset),
          strings != nullptr && std::string_view{strings} == "on");
      if (!parsed || parsed->transaction_control) {
        return MigrationFailure{.kind = MigrationError::MIGRATION_APPLY_FAILED,
                                .extension_name = std::string{extension_name},
                                .migration_file = file.filename,
                                .pg_sqlstate = std::nullopt,
                                .message =
                                    parsed ? "caller-owned migrations cannot "
                                             "contain transaction control"
                                           : parsed.error()};
      }
      statement = file.contents.substr(offset, parsed->length);
      offset += parsed->length;
    }
    auto res = make_result(
        manage_transaction
            ? plinth::db::exec(&conn, file.contents.c_str())
            : plinth::db::exec_params(&conn, statement.c_str(), 0, nullptr,
                                      nullptr, nullptr, nullptr, 0));

    auto status = PQresultStatus(res.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
        status != PGRES_EMPTY_QUERY) {
      auto sqlstate = get_sqlstate(res.get());
      const char* msg = PQresultErrorMessage(res.get());
      std::string err_msg = (msg == nullptr) ? "" : msg;
      while (!err_msg.empty() &&
             (err_msg.back() == '\n' || err_msg.back() == '\r')) {
        err_msg.pop_back();
      }
      if (manage_transaction) {
        (void)exec_ok(conn, "ROLLBACK");
      }
      return MigrationFailure{
          .kind = MigrationError::MIGRATION_APPLY_FAILED,
          .extension_name = std::string{extension_name},
          .migration_file = file.filename,
          .pg_sqlstate = sqlstate,
          .message = "migration " + file.filename + " failed: " + err_msg,
      };
    }
    if (!manage_transaction && PQtransactionStatus(&conn) != PQTRANS_INTRANS) {
      return MigrationFailure{
          .kind = MigrationError::MIGRATION_APPLY_FAILED,
          .extension_name = std::string{extension_name},
          .migration_file = file.filename,
          .pg_sqlstate = std::nullopt,
          .message = "caller-owned migration lost its transaction"};
    }
  } while (!manage_transaction && offset < file.contents.size());

  {
    auto res = exec_with_params(
        conn,
        "INSERT INTO plinth.migrations "
        "(extension_name, migration_file, checksum) "
        "VALUES ($1, $2, $3)",
        {std::string{extension_name}, file.filename, file.checksum_hex});
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      auto sqlstate = get_sqlstate(res.get());
      const char* msg = PQresultErrorMessage(res.get());
      if (manage_transaction) {
        (void)exec_ok(conn, "ROLLBACK");
      }
      return MigrationFailure{
          .kind = MigrationError::MIGRATION_APPLY_FAILED,
          .extension_name = std::string{extension_name},
          .migration_file = file.filename,
          .pg_sqlstate = sqlstate,
          .message = "tracking-row insert failed for " + file.filename + ": " +
                     std::string{msg == nullptr ? "" : msg},
      };
    }
  }

  auto commit_error = manage_transaction ? exec_ok(conn, "COMMIT")
                                         : std::optional<std::string>{};
  if (const auto& err = commit_error; err.has_value()) {
    if (manage_transaction) {
      (void)exec_ok(conn, "ROLLBACK");
    }
    return MigrationFailure{
        .kind = MigrationError::MIGRATION_APPLY_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = file.filename,
        .pg_sqlstate = std::nullopt,
        .message = "COMMIT failed: " + *err,
    };
  }
  return std::nullopt;
}

auto validate_extension_name(std::string_view name) -> bool {
  if (name.size() < 3 || name.size() > 63) {
    return false;
  }
  if (name.front() < 'a' || name.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
  });
}

} // namespace

auto run_migrations(std::string_view extension_name,
                    const fs::path& package_root, PGconn& admin_conn,
                    MigrationTransaction transaction)
    -> std::expected<MigrationReport, MigrationFailure> {
  assert(validate_extension_name(extension_name) &&
         "extension_name must match ^[a-z][a-z0-9_-]{2,62}$");

  if (PQstatus(&admin_conn) != CONNECTION_OK) {
    const char* msg = PQerrorMessage(&admin_conn);
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::DB_CONNECTION_BAD,
        .extension_name = std::string{extension_name},
        .migration_file = std::nullopt,
        .pg_sqlstate = std::nullopt,
        .message = "admin database connection is not OK: " +
                   std::string{msg == nullptr ? "" : msg},
    });
  }

  const bool caller_owned = transaction == MigrationTransaction::CALLER_OWNED;
  if (caller_owned && PQtransactionStatus(&admin_conn) != PQTRANS_INTRANS) {
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::MIGRATION_APPLY_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = std::nullopt,
        .pg_sqlstate = std::nullopt,
        .message = "caller-owned migrations require an active transaction"});
  }
  auto lock_res = try_advisory_lock(admin_conn, extension_name, caller_owned);
  if (!lock_res.has_value()) {
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::ADVISORY_LOCK_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = std::nullopt,
        .pg_sqlstate = std::nullopt,
        .message = "advisory lock query failed: " + lock_res.error(),
    });
  }
  if (!*lock_res) {
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::ADVISORY_LOCK_FAILED,
        .extension_name = std::string{extension_name},
        .migration_file = std::nullopt,
        .pg_sqlstate = std::nullopt,
        .message = "another migration is in progress for ext_" +
                   std::string{extension_name},
    });
  }
  AdvisoryLock lock_guard{admin_conn, extension_name, !caller_owned};

  if (auto fail =
          ensure_schema_and_role(admin_conn, extension_name, !caller_owned);
      fail.has_value()) {
    return std::unexpected(*fail);
  }

  MigrationReport report;

  auto migrations_dir = package_root / "migrations";
  std::error_code ec;
  bool dir_exists = fs::is_directory(migrations_dir, ec);
  if (ec || !dir_exists) {
    return report;
  }

  auto discovery = detail::discover_migrations(migrations_dir, extension_name);
  if (!discovery.has_value()) {
    return std::unexpected(discovery.error());
  }
  for (auto& warn : discovery->warnings) {
    report.warnings.push_back(std::move(warn));
  }

  for (const auto& file : discovery->migrations) {
    plinth::db::OperationScope::checkpoint_current();
    auto stored =
        already_applied_checksum(admin_conn, extension_name, file.filename);
    if (!stored.has_value()) {
      return std::unexpected(stored.error());
    }
    if (stored->has_value()) {
      if (**stored != file.checksum_hex) {
        return std::unexpected(MigrationFailure{
            .kind = MigrationError::CHECKSUM_MISMATCH,
            .extension_name = std::string{extension_name},
            .migration_file = file.filename,
            .pg_sqlstate = std::nullopt,
            .message = "migration " + file.filename +
                       " was modified after being applied "
                       "(expected checksum " +
                       **stored + ", got " + file.checksum_hex + ")",
        });
      }
      report.skipped.push_back(file.filename);
      continue;
    }

    if (auto fail = apply_one(admin_conn, extension_name, file, !caller_owned);
        fail.has_value()) {
      return std::unexpected(*fail);
    }
    report.applied.push_back(file.filename);
  }

  return report;
}

auto drop_schema_and_migrations(std::string_view extension_name,
                                PGconn& admin_conn)
    -> std::expected<void, MigrationFailure> {
  assert(validate_extension_name(extension_name) &&
         "extension_name must match ^[a-z][a-z0-9_-]{2,62}$");

  if (PQstatus(&admin_conn) != CONNECTION_OK) {
    const char* msg = PQerrorMessage(&admin_conn);
    return std::unexpected(MigrationFailure{
        .kind = MigrationError::DB_CONNECTION_BAD,
        .extension_name = std::string{extension_name},
        .migration_file = std::nullopt,
        .pg_sqlstate = std::nullopt,
        .message = "admin database connection is not OK: " +
                   std::string{msg == nullptr ? "" : msg},
    });
  }

  std::string ext = "ext_";
  ext += extension_name;
  std::string role = ext + "_role";

  std::ostringstream sql;
  sql << "BEGIN;\n";
  sql << "  DROP SCHEMA IF EXISTS " << ext << " CASCADE;\n";
  sql << "  DO $$ BEGIN\n"
      << "    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = "
      << pq_escape(admin_conn, role)
      << ") THEN\n"
      // DROP OWNED BY drops any remaining grants (e.g. SELECT on
      // plinth.users) that would otherwise block DROP ROLE with
      // "role has privileges that must be revoked first".
      << "      EXECUTE 'DROP OWNED BY " << role << " CASCADE';\n"
      << "      EXECUTE 'DROP ROLE " << role << "';\n"
      << "    END IF;\n"
      << "  END $$;\n";
  sql << "  DELETE FROM plinth.migrations WHERE extension_name = "
      << pq_escape(admin_conn, std::string{extension_name}) << ";\n";
  sql << "COMMIT;";

  auto res = make_result(plinth::db::exec(&admin_conn, sql.str().c_str()));
  auto status = PQresultStatus(res.get());
  if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
    return {};
  }
  auto sqlstate = get_sqlstate(res.get());
  const char* msg = PQresultErrorMessage(res.get());
  std::string err = (msg == nullptr) ? "" : msg;
  while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) {
    err.pop_back();
  }
  (void)exec_ok(admin_conn, "ROLLBACK");
  return std::unexpected(MigrationFailure{
      .kind = MigrationError::DROP_FAILED,
      .extension_name = std::string{extension_name},
      .migration_file = std::nullopt,
      .pg_sqlstate = sqlstate,
      .message = "teardown of " + ext + " failed: " + err,
  });
}

} // namespace plinth::packages
