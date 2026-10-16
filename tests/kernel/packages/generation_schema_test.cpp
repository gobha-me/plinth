#include "../ws/ws_test_fixture.hpp"
#include "kernel/db/bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <string>

TEST_CASE("generation receipts schema: validated retained bootstrap",
          "[packages][generation_receipts][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  const auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws_test::TestPg pg{cfg.db};
  const auto command = [&](const char* sql) {
    const auto result = pg.exec(sql);
    REQUIRE(result != nullptr);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  };
  const auto retained = [&] {
    plinth::db::bootstrap_schema(
        cfg.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false);
  };
  SECTION("fresh and retained schema are identical and missing table is "
          "recreated empty") {
    REQUIRE_NOTHROW(retained());
    command("DROP TABLE plinth.package_artifact_receipts");
    REQUIRE_NOTHROW(retained());
    const auto result =
        pg.exec("SELECT count(*) FROM plinth.package_artifact_receipts");
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(std::string{PQgetvalue(result.get(), 0, 0)} == "0");
  }
  SECTION("weakened or unvalidated checks are not repaired or trusted") {
    command("ALTER TABLE plinth.package_artifact_receipts DROP CONSTRAINT "
            "package_artifact_receipts_profile_check");
    REQUIRE_THROWS(retained());
  }
  SECTION("NOT VALID check is not trusted") {
    command("ALTER TABLE plinth.package_artifact_receipts DROP CONSTRAINT "
            "package_artifact_receipts_profile_check");
    command("ALTER TABLE plinth.package_artifact_receipts ADD CHECK(profile=1) "
            "NOT VALID");
    REQUIRE_THROWS(retained());
  }
  SECTION("same-shaped varchar columns are incompatible") {
    command("ALTER TABLE plinth.package_artifact_receipts ALTER COLUMN "
            "artifact_digest TYPE varchar(64)");
    REQUIRE_THROWS(retained());
  }
  SECTION("foreign key without cascading lifetime is refused") {
    command("ALTER TABLE plinth.package_artifact_receipts DROP CONSTRAINT "
            "package_artifact_receipts_package_id_fkey");
    command("ALTER TABLE plinth.package_artifact_receipts ADD FOREIGN "
            "KEY(package_id) REFERENCES plinth.packages(id)");
    REQUIRE_THROWS(retained());
  }
  SECTION("public mutation grants are not accepted") {
    command("GRANT INSERT ON plinth.package_artifact_receipts TO PUBLIC");
    REQUIRE_THROWS(retained());
  }
  SECTION("disabled constraint enforcement is refused") {
    command("ALTER TABLE plinth.package_artifact_receipts DISABLE TRIGGER ALL");
    REQUIRE_THROWS(retained());
  }
  SECTION(
      "ordinary inheritance is not mistaken for an isolated receipt table") {
    command("CREATE TABLE plinth.receipt_child() "
            "INHERITS(plinth.package_artifact_receipts)");
    REQUIRE_THROWS(retained());
  }
  SECTION("write rewrite rules are not accepted") {
    command("CREATE RULE receipts_noinsert AS ON INSERT TO "
            "plinth.package_artifact_receipts DO INSTEAD NOTHING");
    REQUIRE_THROWS(retained());
  }
}
