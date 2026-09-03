#include "kernel/realtime/sql_classify.hpp"

#include <cctype>
#include <cstddef>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace plinth::realtime {

namespace {

constexpr std::size_t LOG_SQL_PREFIX = 80;

auto to_lower(char c) -> char {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

auto is_ident_start(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

auto is_ident_rest(char c) -> bool {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

auto is_space(char c) -> bool {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

// Skip ASCII whitespace and SQL comments (`-- line` and `/* block */`)
// at `pos`. Advances past them in-place; leaves `pos` at the first
// non-whitespace, non-comment character (or end-of-string).
auto skip_ws_and_comments(std::string_view s, std::size_t& pos) -> void {
  while (pos < s.size()) {
    if (is_space(s[pos])) {
      ++pos;
      continue;
    }
    if (pos + 1 < s.size() && s[pos] == '-' && s[pos + 1] == '-') {
      pos += 2;
      while (pos < s.size() && s[pos] != '\n') {
        ++pos;
      }
      continue;
    }
    if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
      pos += 2;
      while (pos + 1 < s.size() && (s[pos] != '*' || s[pos + 1] != '/')) {
        ++pos;
      }
      if (pos + 1 < s.size()) {
        pos += 2;
      } else {
        pos = s.size();
      }
      continue;
    }
    break;
  }
}

// Case-insensitive match of `kw` at `pos`, requiring the next character
// (if any) to be a non-ident delimiter so `INSERT` doesn't partial-match
// an ident that starts with `INSERT...`. On success advances `pos` past
// the keyword. ASCII-only per ICD identifier regex.
auto consume_keyword(std::string_view s, std::size_t& pos, std::string_view kw)
    -> bool {
  if (pos + kw.size() > s.size()) {
    return false;
  }
  for (std::size_t i = 0; i < kw.size(); ++i) {
    if (to_lower(s[pos + i]) != to_lower(kw[i])) {
      return false;
    }
  }
  std::size_t after = pos + kw.size();
  if (after < s.size() && is_ident_rest(s[after])) {
    return false;
  }
  pos = after;
  return true;
}

// Consume `[a-z_][a-z0-9_]*` (case-insensitive, lowercased into `out`).
// On success returns true and `pos` advances past the ident. PG
// identifier regex per ICD; quoted idents ("MyTable") not supported.
auto consume_ident_lower(std::string_view s, std::size_t& pos, std::string& out)
    -> bool {
  if (pos >= s.size() || !is_ident_start(s[pos])) {
    return false;
  }
  out.clear();
  out.push_back(to_lower(s[pos]));
  ++pos;
  while (pos < s.size() && is_ident_rest(s[pos])) {
    out.push_back(to_lower(s[pos]));
    ++pos;
  }
  return true;
}

// Shape after an INSERT/UPDATE/DELETE keyword: consume either
// `<schema>.<table>` (qualified) or `<table>` (unqualified — caller
// resolves the implicit schema via ext_name). Returns true on match,
// advancing pos past the identifier(s). `qualified` is set to true
// when a `.` separator was consumed.
auto consume_qualified_or_bare(std::string_view s, std::size_t& pos,
                               std::string& schema_or_table,
                               std::string& table_if_qualified, bool& qualified)
    -> bool {
  if (!consume_ident_lower(s, pos, schema_or_table)) {
    return false;
  }
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    if (!consume_ident_lower(s, pos, table_if_qualified)) {
      return false;
    }
    qualified = true;
  } else {
    qualified = false;
  }
  return true;
}

auto log_skip(std::string_view sql, std::string_view reason) -> void {
  spdlog::trace("realtime coalescer classifier skip: reason={} sql={}", reason,
                sql.substr(0, LOG_SQL_PREFIX));
}

auto detect_extra_statement(std::string_view s, std::size_t pos) -> void {
  while (pos < s.size()) {
    if (s[pos] == ';') {
      std::size_t tail = pos + 1;
      skip_ws_and_comments(s, tail);
      if (tail < s.size()) {
        spdlog::trace("realtime coalescer classifier: multi-statement "
                      "detected; only first classified: {}",
                      s.substr(0, LOG_SQL_PREFIX));
        return;
      }
      return;
    }
    ++pos;
  }
}

} // namespace

auto classify_sql(std::string_view sql, std::string_view ext_name)
    -> std::optional<SqlClass> {
  std::size_t pos = 0;
  skip_ws_and_comments(sql, pos);
  if (pos >= sql.size()) {
    log_skip(sql, "empty");
    return std::nullopt;
  }

  OpKind op_kind{};
  if (consume_keyword(sql, pos, "INSERT")) {
    skip_ws_and_comments(sql, pos);
    if (!consume_keyword(sql, pos, "INTO")) {
      log_skip(sql, "insert_without_into");
      return std::nullopt;
    }
    op_kind = OpKind::INSERT;
  } else if (consume_keyword(sql, pos, "UPDATE")) {
    op_kind = OpKind::UPDATE;
  } else if (consume_keyword(sql, pos, "DELETE")) {
    skip_ws_and_comments(sql, pos);
    if (!consume_keyword(sql, pos, "FROM")) {
      log_skip(sql, "delete_without_from");
      return std::nullopt;
    }
    op_kind = OpKind::DELETE;
  } else {
    // WITH / SELECT / BEGIN / COMMIT / CREATE / ALTER / DROP etc.
    log_skip(sql, "classifier_skip");
    return std::nullopt;
  }

  skip_ws_and_comments(sql, pos);
  std::string first;
  std::string second;
  bool qualified = false;
  if (!consume_qualified_or_bare(sql, pos, first, second, qualified)) {
    log_skip(sql, "missing_table_name");
    return std::nullopt;
  }

  SqlClass cls;
  cls.op_kind = op_kind;
  if (qualified) {
    cls.schema = std::move(first);
    cls.table = std::move(second);
  } else {
    if (ext_name.empty()) {
      // Kernel-scope caller cannot resolve an implicit schema.
      log_skip(sql, "unqualified_kernel_scope");
      return std::nullopt;
    }
    cls.schema = "ext_";
    cls.schema.append(ext_name);
    cls.table = std::move(first);
  }

  detect_extra_statement(sql, pos);

  // §Defense-in-depth — mismatch is a bug, not a security gate. Log
  // and still return the extracted tuple; ICD-0.4.3 search_path +
  // role + capability gate is the primary isolation boundary.
  if (!ext_name.empty()) {
    std::string expected = "ext_";
    expected.append(ext_name);
    if (cls.schema != expected) {
      spdlog::warn("realtime coalescer classifier: schema mismatch "
                   "extracted={} expected={} sql={}",
                   cls.schema, expected, sql.substr(0, LOG_SQL_PREFIX));
    }
  }

  return cls;
}

} // namespace plinth::realtime
