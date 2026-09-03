// SPDX-License-Identifier: MIT
//
// ICD-0.5.1 §SQL Classification unit coverage. Exercises every
// supported shape (INSERT/UPDATE/DELETE qualified + unqualified), the
// skip cases (DDL, SELECT, WITH, empty, multi-statement), and the
// §Defense-in-depth mismatch warn path. E.01 from the ICD test matrix
// lives here (DDL rejection).

#include "kernel/realtime/sql_classify.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string_view>

using plinth::realtime::classify_sql;
using plinth::realtime::OpKind;

namespace {

auto require_class(std::optional<plinth::realtime::SqlClass> c,
                   std::string_view schema, std::string_view table, OpKind kind)
    -> void {
  REQUIRE(c.has_value());
  CHECK(c->schema == schema);
  CHECK(c->table == table);
  CHECK(c->op_kind == kind);
}

} // namespace

TEST_CASE("classify_sql qualified INSERT VALUES",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql(
      "INSERT INTO ext_notes.notes (id, title) VALUES ('n1', 't')", "notes");
  require_class(c, "ext_notes", "notes", OpKind::INSERT);
}

TEST_CASE("classify_sql qualified INSERT SELECT",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql(
      "INSERT INTO ext_notes.tags SELECT id FROM ext_notes.notes", "notes");
  require_class(c, "ext_notes", "tags", OpKind::INSERT);
}

TEST_CASE("classify_sql qualified UPDATE", "[realtime][coalescer][unit]") {
  auto c = classify_sql(
      "UPDATE ext_notes.notes SET title = 'x' WHERE id = 'n1'", "notes");
  require_class(c, "ext_notes", "notes", OpKind::UPDATE);
}

TEST_CASE("classify_sql qualified DELETE", "[realtime][coalescer][unit]") {
  auto c = classify_sql("DELETE FROM ext_notes.notes WHERE id = 'n1'", "notes");
  require_class(c, "ext_notes", "notes", OpKind::DELETE);
}

TEST_CASE("classify_sql unqualified resolves via ext_name",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("INSERT INTO notes (id) VALUES ('x')", "notes");
  require_class(c, "ext_notes", "notes", OpKind::INSERT);
}

TEST_CASE("classify_sql unqualified UPDATE resolves via ext_name",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("UPDATE notes SET title='x'", "notes");
  require_class(c, "ext_notes", "notes", OpKind::UPDATE);
}

TEST_CASE("classify_sql unqualified DELETE resolves via ext_name",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("DELETE FROM notes WHERE id='n1'", "notes");
  require_class(c, "ext_notes", "notes", OpKind::DELETE);
}

TEST_CASE("classify_sql unqualified with empty ext_name skips",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("INSERT INTO notes (id) VALUES ('x')", "");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql case insensitive", "[realtime][coalescer][unit]") {
  auto c =
      classify_sql("insert into ext_notes.notes (id) values ('x')", "notes");
  require_class(c, "ext_notes", "notes", OpKind::INSERT);
}

TEST_CASE("classify_sql strips leading whitespace + line comment",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("   -- note: insert below\n"
                        "   INSERT INTO ext_notes.notes (id) VALUES ('x')",
                        "notes");
  require_class(c, "ext_notes", "notes", OpKind::INSERT);
}

TEST_CASE("classify_sql strips block comment", "[realtime][coalescer][unit]") {
  auto c =
      classify_sql("/* hello */ UPDATE ext_notes.notes SET title='x'", "notes");
  require_class(c, "ext_notes", "notes", OpKind::UPDATE);
}

TEST_CASE("E.01 classify_sql rejects DDL (CREATE INDEX)",
          "[realtime][coalescer][unit][errors]") {
  auto c = classify_sql("CREATE INDEX idx_notes_id ON ext_notes.notes (id)",
                        "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql rejects DDL (CREATE TABLE)",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("CREATE TABLE ext_notes.notes (id TEXT)", "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql rejects DDL (ALTER TABLE)",
          "[realtime][coalescer][unit]") {
  auto c =
      classify_sql("ALTER TABLE ext_notes.notes ADD COLUMN x TEXT", "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql rejects DDL (DROP TABLE)",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("DROP TABLE ext_notes.notes", "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql rejects SELECT", "[realtime][coalescer][unit]") {
  auto c = classify_sql("SELECT 1", "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql rejects WITH (CTE write)",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql(
      "WITH x AS (SELECT 1) INSERT INTO ext_notes.notes SELECT * FROM x",
      "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql rejects BEGIN/COMMIT", "[realtime][coalescer][unit]") {
  CHECK_FALSE(classify_sql("BEGIN", "notes").has_value());
  CHECK_FALSE(classify_sql("COMMIT", "notes").has_value());
  CHECK_FALSE(classify_sql("SET search_path = x", "notes").has_value());
}

TEST_CASE("classify_sql rejects empty + whitespace-only",
          "[realtime][coalescer][unit]") {
  CHECK_FALSE(classify_sql("", "notes").has_value());
  CHECK_FALSE(classify_sql("    \n\t ", "notes").has_value());
  CHECK_FALSE(classify_sql("-- just a comment", "notes").has_value());
}

TEST_CASE("classify_sql INSERT without INTO skips",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("INSERT ext_notes.notes VALUES ('x')", "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql DELETE without FROM skips",
          "[realtime][coalescer][unit]") {
  auto c = classify_sql("DELETE ext_notes.notes", "notes");
  REQUIRE_FALSE(c.has_value());
}

TEST_CASE("classify_sql multi-statement classifies first",
          "[realtime][coalescer][unit]") {
  // Debug log fires; first stmt returns. Tail statements are warned
  // but ignored — the warn path is non-fatal.
  auto c = classify_sql("INSERT INTO ext_notes.notes (id) VALUES ('x'); "
                        "UPDATE ext_notes.tags SET name='x'",
                        "notes");
  require_class(c, "ext_notes", "notes", OpKind::INSERT);
}

TEST_CASE("classify_sql mismatch returns tuple + warns",
          "[realtime][coalescer][unit]") {
  // §Defense-in-depth — mismatch is logged but the classifier still
  // returns the extracted (schema, table, op_kind) because
  // ICD-0.4.3's search_path + role + capability gate is the primary
  // isolation boundary, not this check.
  auto c =
      classify_sql("INSERT INTO ext_other.notes (id) VALUES ('x')", "notes");
  require_class(c, "ext_other", "notes", OpKind::INSERT);
}
