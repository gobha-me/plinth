#include "kernel/packages/migrations_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using plinth::packages::MigrationError;
using plinth::packages::detail::check_qualified_ddl;
using plinth::packages::detail::discover_migrations;
using plinth::packages::detail::parse_migration_filename;
using plinth::packages::detail::sha256_hex;
using plinth::packages::detail::strip_sql_noise;
using plinth::packages::detail::strip_utf8_bom;

namespace {

class TempDir {
 public:
  TempDir() {
    static std::atomic<std::uint64_t> counter{0};
    auto n = counter.fetch_add(1, std::memory_order_relaxed);
    dir = fs::temp_directory_path() /
          ("plinth_migrations_test_" + std::to_string(n));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  TempDir(const TempDir&) = delete;
  auto operator=(const TempDir&) -> TempDir& = delete;
  TempDir(TempDir&&) = delete;
  auto operator=(TempDir&&) -> TempDir& = delete;

  [[nodiscard]] auto path() const -> const fs::path& { return dir; }

  void write(const std::string& rel, const std::string& contents) const {
    auto p = dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

 private:
  fs::path dir;
};

} // namespace

TEST_CASE("parse_migration_filename: valid sequences",
          "[packages][migrations][unit]") {
  REQUIRE(parse_migration_filename("001_create.sql") == 1);
  REQUIRE(parse_migration_filename("002_add_index.sql") == 2);
  REQUIRE(parse_migration_filename("010_foo.sql") == 10);
  REQUIRE(parse_migration_filename("9999_foo.sql") == 9999);
  REQUIRE(parse_migration_filename("0_noop.sql") == 0);
  REQUIRE(parse_migration_filename("001_a-b-c.sql") == 1);
}

TEST_CASE("parse_migration_filename: rejects malformed names",
          "[packages][migrations][unit]") {
  REQUIRE_FALSE(parse_migration_filename("001.sql").has_value());
  REQUIRE_FALSE(parse_migration_filename("001-no-underscore.sql").has_value());
  REQUIRE_FALSE(parse_migration_filename("01a_foo.sql").has_value());
  REQUIRE_FALSE(parse_migration_filename("abc_foo.sql").has_value());
  REQUIRE_FALSE(parse_migration_filename("001_foo.txt").has_value());
  REQUIRE_FALSE(parse_migration_filename("001_foo").has_value());
  REQUIRE_FALSE(parse_migration_filename("001_FOO.sql").has_value());
  REQUIRE_FALSE(parse_migration_filename("001_Foo.sql").has_value());
  REQUIRE_FALSE(parse_migration_filename("001_foo.SQL").has_value());
  REQUIRE_FALSE(parse_migration_filename("_001_foo.sql").has_value());
  REQUIRE_FALSE(parse_migration_filename("").has_value());
}

TEST_CASE("strip_utf8_bom: removes single leading BOM",
          "[packages][migrations][unit]") {
  std::string with_bom;
  with_bom.push_back(static_cast<char>(0xEFU));
  with_bom.push_back(static_cast<char>(0xBBU));
  with_bom.push_back(static_cast<char>(0xBFU));
  with_bom += "SELECT 1;";
  strip_utf8_bom(with_bom);
  REQUIRE(with_bom == "SELECT 1;");

  std::string no_bom = "SELECT 1;";
  strip_utf8_bom(no_bom);
  REQUIRE(no_bom == "SELECT 1;");

  std::string short_1 = "\xEF";
  strip_utf8_bom(short_1);
  REQUIRE(short_1 == "\xEF");
}

TEST_CASE("sha256_hex: deterministic hex digest",
          "[packages][migrations][unit]") {
  // empty input
  REQUIRE(sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  // known vector
  REQUIRE(sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // reproducibility
  REQUIRE(sha256_hex("CREATE TABLE foo();") ==
          sha256_hex("CREATE TABLE foo();"));
}

TEST_CASE("discover_migrations: numeric not lexicographic",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("001_a.sql", "SELECT 1;");
  tmp.write("002_b.sql", "SELECT 2;");
  tmp.write("010_c.sql", "SELECT 10;");

  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE(result.has_value());
  REQUIRE(result->migrations.size() == 3);
  REQUIRE(result->migrations[0].sequence == 1);
  REQUIRE(result->migrations[1].sequence == 2);
  REQUIRE(result->migrations[2].sequence == 10);
  // Gap between 2 and 10 is legal (numbers may be reserved for squashed
  // migrations). Whether a warning is emitted is a separate concern
  // exercised by the "gap emits warning" test.
}

TEST_CASE("discover_migrations: duplicate sequence fails",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("001_a.sql", "SELECT 1;");
  tmp.write("001_b.sql", "SELECT 2;");

  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == MigrationError::DUPLICATE_SEQUENCE);
}

TEST_CASE("discover_migrations: invalid filename fails",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("001.sql", "SELECT 1;");

  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == MigrationError::INVALID_FILENAME);
}

TEST_CASE("discover_migrations: non-matching files ignored",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("README.md", "hello");
  tmp.write(".gitkeep", "");
  tmp.write("backup~", "");
  tmp.write("001_foo.txt", "not a migration");
  tmp.write("001_real.sql", "SELECT 1;");

  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE(result.has_value());
  REQUIRE(result->migrations.size() == 1);
  REQUIRE(result->migrations[0].filename == "001_real.sql");
}

TEST_CASE("discover_migrations: gap emits warning, not error",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("001_a.sql", "a");
  tmp.write("004_b.sql", "b");

  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE(result.has_value());
  REQUIRE(result->migrations.size() == 2);
  REQUIRE(result->warnings.size() == 1);
  REQUIRE(result->warnings[0].kind == "migration-gap");
  REQUIRE(result->warnings[0].detail.find('2') != std::string::npos);
  REQUIRE(result->warnings[0].detail.find('3') != std::string::npos);
}

TEST_CASE("discover_migrations: BOM stripped before checksum",
          "[packages][migrations][unit]") {
  TempDir tmp;
  std::string with_bom;
  with_bom.push_back(static_cast<char>(0xEFU));
  with_bom.push_back(static_cast<char>(0xBBU));
  with_bom.push_back(static_cast<char>(0xBFU));
  with_bom += "SELECT 1;";
  tmp.write("001_a.sql", with_bom);

  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE(result.has_value());
  REQUIRE(result->migrations.size() == 1);
  REQUIRE(result->migrations[0].checksum_hex == sha256_hex("SELECT 1;"));
  REQUIRE(result->migrations[0].contents == "SELECT 1;");
}

TEST_CASE("discover_migrations: empty dir returns empty report",
          "[packages][migrations][unit]") {
  TempDir tmp;
  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE(result.has_value());
  REQUIRE(result->migrations.empty());
  REQUIRE(result->warnings.empty());
}

TEST_CASE("discover_migrations: three in order",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("003_c.sql", "c");
  tmp.write("001_a.sql", "a");
  tmp.write("002_b.sql", "b");

  auto result = discover_migrations(tmp.path(), "ext_test");
  REQUIRE(result.has_value());
  REQUIRE(result->migrations.size() == 3);
  REQUIRE(result->migrations[0].filename == "001_a.sql");
  REQUIRE(result->migrations[1].filename == "002_b.sql");
  REQUIRE(result->migrations[2].filename == "003_c.sql");
}

// ── strip_sql_noise: comments, string literals, dollar quotes ─────────

TEST_CASE("strip_sql_noise: line comment blanked",
          "[packages][migrations][unit]") {
  auto out = strip_sql_noise("SELECT 1; -- CREATE TABLE foo\nSELECT 2;");
  REQUIRE(out.find("CREATE") == std::string::npos);
  REQUIRE(out.find("SELECT 1;") != std::string::npos);
  REQUIRE(out.find("SELECT 2;") != std::string::npos);
  REQUIRE(out.find('\n') != std::string::npos);
}

TEST_CASE("strip_sql_noise: block comment blanked, nesting respected",
          "[packages][migrations][unit]") {
  auto out = strip_sql_noise(
      "/* outer /* CREATE TABLE inner */ still inside */ AFTER");
  REQUIRE(out.find("CREATE") == std::string::npos);
  REQUIRE(out.find("AFTER") != std::string::npos);
}

TEST_CASE("strip_sql_noise: single-quoted literal blanked",
          "[packages][migrations][unit]") {
  auto out = strip_sql_noise("SELECT 'CREATE TABLE foo';");
  REQUIRE(out.find("CREATE") == std::string::npos);
  REQUIRE(out.find("SELECT") != std::string::npos);
}

TEST_CASE("strip_sql_noise: doubled quote inside string preserved as data",
          "[packages][migrations][unit]") {
  auto out = strip_sql_noise("SELECT 'a''b CREATE TABLE foo';");
  REQUIRE(out.find("CREATE") == std::string::npos);
}

TEST_CASE("strip_sql_noise: dollar-quoted body blanked",
          "[packages][migrations][unit]") {
  auto out = strip_sql_noise("CREATE FUNCTION ext_n.f() RETURNS int AS $$ "
                             "CREATE TABLE inner_evil; "
                             "$$ LANGUAGE sql;");
  auto first = out.find("CREATE");
  REQUIRE(first != std::string::npos);
  auto second = out.find("CREATE", first + 6);
  REQUIRE(second == std::string::npos);
}

TEST_CASE("strip_sql_noise: tagged dollar-quote body blanked",
          "[packages][migrations][unit]") {
  auto out = strip_sql_noise("DO $tag$ CREATE TABLE inner; $tag$;");
  REQUIRE(out.find("CREATE") == std::string::npos);
}

// ── check_qualified_ddl: positive cases ───────────────────────────────

TEST_CASE("check_qualified_ddl: bare CREATE TABLE rejected",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("CREATE TABLE notes (id INT);", "notes");
  REQUIRE(err.has_value());
  REQUIRE(err->find("notes") != std::string::npos);
  REQUIRE(err->find("ext_notes") != std::string::npos);
}

TEST_CASE("check_qualified_ddl: bare CREATE TABLE IF NOT EXISTS rejected",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("CREATE TABLE IF NOT EXISTS notes (id INT);",
                                 "notes");
  REQUIRE(err.has_value());
}

TEST_CASE("check_qualified_ddl: wrong-schema CREATE TABLE rejected",
          "[packages][migrations][unit]") {
  auto err =
      check_qualified_ddl("CREATE TABLE plinth.notes (id INT);", "notes");
  REQUIRE(err.has_value());
  REQUIRE(err->find("plinth.notes") != std::string::npos);
}

TEST_CASE("check_qualified_ddl: prefix-only-collision rejected",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("CREATE TABLE ext_notes_other.foo (id INT);",
                                 "notes");
  REQUIRE(err.has_value());
}

TEST_CASE("check_qualified_ddl: CREATE VIEW rejected when unqualified",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("CREATE VIEW v AS SELECT 1;", "notes");
  REQUIRE(err.has_value());
}

TEST_CASE("check_qualified_ddl: CREATE MATERIALIZED VIEW rejected",
          "[packages][migrations][unit]") {
  auto err =
      check_qualified_ddl("CREATE MATERIALIZED VIEW mv AS SELECT 1;", "notes");
  REQUIRE(err.has_value());
}

TEST_CASE("check_qualified_ddl: CREATE SEQUENCE rejected",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("CREATE SEQUENCE s START 1;", "notes");
  REQUIRE(err.has_value());
}

TEST_CASE("check_qualified_ddl: CREATE TYPE rejected",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("CREATE TYPE t AS ENUM ('a','b');", "notes");
  REQUIRE(err.has_value());
}

TEST_CASE("check_qualified_ddl: CREATE FUNCTION rejected",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl(
      "CREATE FUNCTION f() RETURNS INT AS $$ SELECT 1 $$ LANGUAGE sql;",
      "notes");
  REQUIRE(err.has_value());
}

TEST_CASE("check_qualified_ddl: CREATE OR REPLACE FUNCTION rejected",
          "[packages][migrations][unit]") {
  auto err =
      check_qualified_ddl("CREATE OR REPLACE FUNCTION f() RETURNS INT AS "
                          "$$ SELECT 1 $$ LANGUAGE sql;",
                          "notes");
  REQUIRE(err.has_value());
}

// ── check_qualified_ddl: negative cases (should pass) ─────────────────

TEST_CASE("check_qualified_ddl: qualified CREATE TABLE accepted",
          "[packages][migrations][unit]") {
  auto err =
      check_qualified_ddl("CREATE TABLE ext_notes.notes (id INT);", "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE("check_qualified_ddl: qualified CREATE TABLE IF NOT EXISTS accepted",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl(
      "CREATE TABLE IF NOT EXISTS ext_notes.notes (id INT);", "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE("check_qualified_ddl: quoted-schema CREATE TABLE accepted",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl(
      R"(CREATE TABLE "ext_notes"."notes" (id INT);)", "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE("check_qualified_ddl: comment containing CREATE TABLE not flagged",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("-- example: CREATE TABLE notes\n"
                                 "CREATE TABLE ext_notes.notes (id INT);",
                                 "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE("check_qualified_ddl: literal containing CREATE TABLE not flagged",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("INSERT INTO ext_notes.docs (body) "
                                 "VALUES ('CREATE TABLE notes (id INT)');",
                                 "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE(
    "check_qualified_ddl: dollar-quoted body containing CREATE not flagged",
    "[packages][migrations][unit]") {
  auto err =
      check_qualified_ddl("CREATE FUNCTION ext_notes.f() RETURNS INT AS $$ "
                          "CREATE TABLE inner_should_be_ignored; SELECT 1; "
                          "$$ LANGUAGE sql;",
                          "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE("check_qualified_ddl: CREATE INDEX not enforced",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl(
      "CREATE INDEX idx_notes_id ON ext_notes.notes(id);", "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE("check_qualified_ddl: ALTER + INSERT not flagged",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl(
      "ALTER TABLE ext_notes.notes ADD COLUMN flagged BOOLEAN; "
      "INSERT INTO ext_notes.notes (id) VALUES (1);",
      "notes");
  REQUIRE_FALSE(err.has_value());
}

TEST_CASE("check_qualified_ddl: empty SQL accepted",
          "[packages][migrations][unit]") {
  REQUIRE_FALSE(check_qualified_ddl("", "notes").has_value());
  REQUIRE_FALSE(
      check_qualified_ddl("-- only a comment\n", "notes").has_value());
  REQUIRE_FALSE(check_qualified_ddl("SELECT 1;", "notes").has_value());
}

TEST_CASE("check_qualified_ddl: multi-statement, first qualified second not",
          "[packages][migrations][unit]") {
  auto err = check_qualified_ddl("CREATE TABLE ext_notes.a (id INT);\n"
                                 "CREATE TABLE b (id INT);",
                                 "notes");
  REQUIRE(err.has_value());
}

// ── discover_migrations: integrates the guard ─────────────────────────

TEST_CASE("discover_migrations: unqualified DDL fails with UNQUALIFIED_DDL",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("001_a.sql", "CREATE TABLE notes (id INT);");

  auto result = discover_migrations(tmp.path(), "notes");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == MigrationError::UNQUALIFIED_DDL);
  REQUIRE(result.error().migration_file.has_value());
  REQUIRE(*result.error().migration_file == "001_a.sql");
}

TEST_CASE("discover_migrations: qualified DDL accepted",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("001_a.sql", "CREATE TABLE ext_notes.foo (id INT);");

  auto result = discover_migrations(tmp.path(), "notes");
  REQUIRE(result.has_value());
  REQUIRE(result->migrations.size() == 1);
}

TEST_CASE("discover_migrations: duplicate-sequence reported before "
          "qualified-DDL",
          "[packages][migrations][unit]") {
  TempDir tmp;
  tmp.write("001_a.sql", "CREATE TABLE ext_notes.a (id INT);");
  tmp.write("001_b.sql", "CREATE TABLE notes (id INT);");

  auto result = discover_migrations(tmp.path(), "notes");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == MigrationError::DUPLICATE_SEQUENCE);
}
