#include "kernel/packages/generation_receipts.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include <catch2/catch_test_macros.hpp>

#include <libpq-fe.h>

#include <memory>
#include <string>

namespace {
namespace generation = plinth::packages::generation;
namespace names = plinth::packages::name_operation;
using Operation = names::NameOperation;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto connect(const plinth::Config::Database& configuration)
    -> std::shared_ptr<PGconn> {
  auto connection = std::shared_ptr<PGconn>(
      plinth::db::connect(plinth::db::connection_info(configuration).c_str()),
      PQfinish);
  REQUIRE(connection != nullptr);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  return connection;
}
auto command(PGconn& connection, const char* sql) -> void {
  const Result result(plinth::db::exec(&connection, sql), PQclear);
  REQUIRE(result != nullptr);
  REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
}
auto rows(PGconn& connection) -> std::string {
  const Result result(
      plinth::db::exec(
          &connection,
          "SELECT count(*)::text FROM plinth.package_artifact_receipts"),
      PQclear);
  REQUIRE(result != nullptr);
  REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(result.get()) == 1);
  return PQgetvalue(result.get(), 0, 0);
}
} // namespace

TEST_CASE("generation receipts: write-once transaction and lock ownership",
          "[packages][generation_receipts][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  const auto configuration = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(configuration.db);
  plinth::ws_test::TestPg observer{configuration.db};
  const auto seeded = observer.exec_params(
      "INSERT INTO plinth.packages(name,version,state,provenance,manifest_json,"
      "entry_point,manifest_checksum) VALUES($1,$2,'ACTIVATING','user','{}',"
      "'server/main.js',$3) RETURNING id::text",
      {"receiptfixture", "1.0.0", std::string(64, '0')});
  REQUIRE(PQresultStatus(seeded.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(seeded.get()) == 1);
  const generation::GenerationKey key{.package_id =
                                          PQgetvalue(seeded.get(), 0, 0),
                                      .name = "receiptfixture",
                                      .version = "1.0.0",
                                      .profile = 1};
  const generation::GenerationReceipt receipt{
      .key = key,
      .artifact_digest = std::string(64, 'a'),
      .declarations_digest = std::string(64, 'b'),
      .record_count = 1,
      .content_bytes = 3,
      .metadata_bytes = 64};
  auto connection = connect(configuration.db);
  auto other = connect(configuration.db);
  REQUIRE(names::lock_key(key.name) == "plinth.packages.receiptfixture");
  auto operation = Operation::acquire(connection, key.name);
  REQUIRE(operation.has_value());
  REQUIRE(operation->assert_owned().has_value());

  SECTION("same key contention and borrowed proof never reacquire or unlock") {
    const auto contended = Operation::acquire(other, key.name);
    REQUIRE_FALSE(contended.has_value());
    REQUIRE(contended.error() == names::NameError::BUSY);
    const auto recursive = Operation::acquire(connection, key.name);
    REQUIRE_FALSE(recursive.has_value());
    REQUIRE(recursive.error() == names::NameError::INVALID);
    const auto wrong_session = Operation::borrow_existing(*other, key.name);
    REQUIRE_FALSE(wrong_session.has_value());
    REQUIRE(wrong_session.error() == names::NameError::NOT_OWNED);
    {
      auto borrowed = Operation::borrow_existing(*connection, key.name);
      REQUIRE(borrowed.has_value());
      REQUIRE(borrowed->assert_owned().has_value());
      const auto cannot_release = borrowed->release_checked();
      REQUIRE_FALSE(cannot_release.has_value());
      REQUIRE(cannot_release.error() == names::NameError::NOT_OWNED);
    }
    REQUIRE(operation->assert_owned().has_value());
    REQUIRE(operation->release_checked().has_value());
    auto replacement = Operation::acquire(other, key.name);
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->release_checked().has_value());
    const auto borrowed_after_release =
        Operation::borrow_existing(*connection, key.name);
    REQUIRE_FALSE(borrowed_after_release.has_value());
  }
  SECTION(
      "caller transaction owns visibility rollback commit and idempotence") {
    const auto missing = generation::read_receipt(*operation, key);
    REQUIRE(missing.has_value());
    REQUIRE_FALSE(missing->has_value());
    const auto outside =
        generation::insert_matching_receipt(*operation, receipt);
    REQUIRE_FALSE(outside.has_value());
    REQUIRE(outside.error() == generation::ReceiptError::TRANSACTION_REQUIRED);
    REQUIRE(PQtransactionStatus(connection.get()) == PQTRANS_IDLE);
    command(*connection, "BEGIN");
    REQUIRE(
        generation::insert_matching_receipt(*operation, receipt).has_value());
    REQUIRE(PQtransactionStatus(connection.get()) == PQTRANS_INTRANS);
    REQUIRE(rows(*observer.conn) == "0");
    command(*connection, "ROLLBACK");
    REQUIRE(rows(*observer.conn) == "0");
    const auto rolled_back = generation::read_receipt(*operation, key);
    REQUIRE(rolled_back.has_value());
    REQUIRE_FALSE(rolled_back->has_value());
    command(*connection, "BEGIN");
    REQUIRE(
        generation::insert_matching_receipt(*operation, receipt).has_value());
    command(*connection, "COMMIT");
    REQUIRE(rows(*observer.conn) == "1");
    const auto committed = generation::read_receipt(*operation, key);
    REQUIRE(committed.has_value());
    REQUIRE(committed->has_value());
    REQUIRE(**committed == receipt);
    command(*connection, "BEGIN");
    REQUIRE(
        generation::insert_matching_receipt(*operation, receipt).has_value());
    auto changed = receipt;
    changed.content_bytes = 4;
    const auto conflict =
        generation::insert_matching_receipt(*operation, changed);
    REQUIRE_FALSE(conflict.has_value());
    REQUIRE(conflict.error() == generation::ReceiptError::CONFLICT);
    REQUIRE(PQtransactionStatus(connection.get()) == PQTRANS_INTRANS);
    command(*connection, "ROLLBACK");
    REQUIRE(rows(*observer.conn) == "1");
  }
  SECTION(
      "forged fields mismatched generation and stale borrowed token refuse") {
    auto wrong = key;
    wrong.version = "1.0.1";
    const auto mismatch = generation::read_receipt(*operation, wrong);
    REQUIRE_FALSE(mismatch.has_value());
    REQUIRE(mismatch.error() == generation::ReceiptError::GENERATION_MISMATCH);
    wrong = key;
    wrong.name = "other";
    const auto wrong_name = generation::read_receipt(*operation, wrong);
    REQUIRE_FALSE(wrong_name.has_value());
    REQUIRE(wrong_name.error() == generation::ReceiptError::INVALID);
    command(*connection, "BEGIN");
    auto invalid = receipt;
    invalid.artifact_digest.assign(64, 'A');
    const auto uppercase =
        generation::insert_matching_receipt(*operation, invalid);
    REQUIRE_FALSE(uppercase.has_value());
    REQUIRE(uppercase.error() == generation::ReceiptError::INVALID);
    invalid = receipt;
    invalid.metadata_bytes = 31;
    REQUIRE_FALSE(generation::insert_matching_receipt(*operation, invalid));
    invalid = receipt;
    invalid.content_bytes = std::uint64_t{50} * 1024 * 1024 + 1;
    REQUIRE_FALSE(generation::insert_matching_receipt(*operation, invalid));
    invalid = receipt;
    invalid.key.profile = 2;
    REQUIRE_FALSE(generation::insert_matching_receipt(*operation, invalid));
    command(*connection, "ROLLBACK");
    REQUIRE(rows(*observer.conn) == "0");
    auto borrowed = Operation::borrow_existing(*connection, key.name);
    REQUIRE(borrowed.has_value());
    REQUIRE(operation->release_checked().has_value());
    const auto stale = generation::read_receipt(*borrowed, key);
    REQUIRE_FALSE(stale.has_value());
    REQUIRE(stale.error() == generation::ReceiptError::LOCK_NOT_OWNED);
  }
  SECTION("owning token retains its connection until real release") {
    REQUIRE(operation->release_checked().has_value());
    const std::weak_ptr<PGconn> weak = connection;
    {
      auto retained = Operation::acquire(std::move(connection), key.name);
      REQUIRE(retained.has_value());
      REQUIRE_FALSE(weak.expired());
      REQUIRE(retained->assert_owned().has_value());
    }
    REQUIRE(weak.expired());
    auto after = Operation::acquire(other, key.name);
    REQUIRE(after.has_value());
    REQUIRE(after->release_checked().has_value());
  }
}
