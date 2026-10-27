#include "kernel/packages/http_claims.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/extension_http/codec.hpp"
#include "kernel/packages/generation_receipts.hpp"
#include "kernel/packages/http_install_authorization.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/rbac/enforcement.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace claims = plinth::packages::http_claims;
namespace packages = plinth::packages;
namespace names = packages::name_operation;
namespace generation = packages::generation;
using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
using Json = nlohmann::ordered_json;
using namespace std::chrono_literals;
constexpr auto EPOCH = "12345678-1234-1234-1234-123456789abc";
constexpr auto OTHER = "22345678-1234-1234-1234-123456789abc";

auto command(PGconn& connection, const std::string& sql) -> void {
  const PgResult result{plinth::db::exec(&connection, sql.c_str()), PQclear};
  REQUIRE(result != nullptr);
  REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
}
auto scalar(PGconn& connection, const std::string& sql) -> std::string {
  const PgResult result{plinth::db::exec(&connection, sql.c_str()), PQclear};
  REQUIRE(result != nullptr);
  REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(result.get()) == 1);
  return PQgetvalue(result.get(), 0, 0);
}
auto connect(const plinth::Config::Database& configuration)
    -> std::shared_ptr<PGconn> {
  auto connection = std::shared_ptr<PGconn>(
      plinth::db::connect(plinth::db::connection_info(configuration).c_str()),
      PQfinish);
  REQUIRE(connection != nullptr);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  return connection;
}
struct Input {
  std::string name;
  std::string version;
  std::string raw;
  packages::HttpDeclaration declaration;
  // Explicit synthetic binding bytes. These PG store fixtures do NOT claim a
  // ZIP install/capture, actual filter execution, physical adapter or route.
  std::vector<std::byte> zip{std::byte{'P'}, std::byte{'K'}, std::byte{3},
                             std::byte{4}};
  Input(std::string name_in, std::string prefix,
        std::string version_in = "1.0.0", std::string frontend = {})
      : name(std::move(name_in)), version(std::move(version_in)) {
    Json manifest{{"name", name},
                  {"version", version},
                  {"description", "HTTP claim store fixture"},
                  {"author", "plinth-tests"},
                  {"license", "MIT"},
                  {"entry_point", "server/main.js"},
                  {"http_prefixes", Json::array({prefix})},
                  {"handler_mode", "quickjs"}};
    if (!frontend.empty())
      manifest["frontend"] = {{"mount", frontend}, {"entry", "index.html"}};
    raw = manifest.dump();
    auto parsed = packages::HttpDeclaration::parse(raw);
    REQUIRE(parsed.value.has_value());
    declaration = std::move(*parsed.value);
  }
  auto authorize(std::string_view actor,
                 packages::HttpInstallOperation operation =
                     packages::HttpInstallOperation::INSTALL,
                 std::string_view predecessor = {}) const
      -> packages::AuthorizedHttpSubmission {
    // Trusted request-attribute snapshot fixture, through the SAME production
    // opaque-proof factory; no proof constructor or caller permission boolean.
    auto request = drogon::HttpRequest::newHttpRequest();
    request->attributes()->insert(plinth::auth::ATTR_USER_ID,
                                  std::string{actor});
    request->attributes()->insert(plinth::auth::ATTR_AUTH_TYPE,
                                  std::string{"session"});
    request->attributes()->insert(plinth::auth::ATTR_CREDENTIAL_SOURCE,
                                  std::string{"cookie"});
    request->attributes()->insert(
        plinth::rbac::ATTR_EFFECTIVE_RULES,
        std::vector<std::string>{"packages.install",
                                 "packages.install.with_http_prefixes"});
    request->attributes()->insert(plinth::rbac::ATTR_PERMISSION_GRANTED, true);
    const packages::HttpInstallAuthorizationRequest input{
        zip,     raw,       declaration, name,
        version, operation, predecessor, packages::Provenance::USER};
    auto authorized =
        packages::make_http_install_authorizer(request, zip)(input);
    REQUIRE(authorized.has_value());
    return std::move(*authorized);
  }
};
struct Fixture {
  plinth::Config config = plinth::ws_test::test_config();
  std::unique_ptr<plinth::ws_test::TestPg> observer;
  std::shared_ptr<PGconn> connection;
  std::string actor;
  Fixture() {
    plinth::ws_test::reset_schema(config.db);
    observer = std::make_unique<plinth::ws_test::TestPg>(config.db);
    actor = plinth::ws_test::insert_user(*observer, "claims-owner",
                                         "fake-claims-password");
    connection = connect(config.db);
    command(*connection, "BEGIN");
    REQUIRE(claims::ensure_schema(*connection));
    command(*connection, "COMMIT");
  }
  auto seed(const Input& input, const packages::AuthorizedHttpSubmission& auth,
            std::string_view state = "VALIDATING",
            std::string_view frontend = {}) -> std::string {
    auto inserted = observer->exec_params(
        "INSERT INTO "
        "plinth.packages(name,version,state,provenance,manifest_json,entry_"
        "point,manifest_checksum,"
        "installed_by_user_id,supersedes_id,frontend_mount,frontend_entry) "
        "VALUES($1,$2,$3,'user',$4::jsonb,'server/"
        "main.js',$5,$6::uuid,NULLIF($7,'')::uuid,NULLIF($8,''),"
        "CASE WHEN $8='' THEN NULL ELSE 'index.html' END) RETURNING id::text",
        {input.name, input.version, std::string{state}, input.raw,
         auth.raw_manifest_sha256(), actor, auth.predecessor_id(),
         std::string{frontend}});
    REQUIRE(PQresultStatus(inserted.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(inserted.get()) == 1);
    return PQgetvalue(inserted.get(), 0, 0);
  }
};
auto acquire(const std::shared_ptr<PGconn>& connection, const Input& input)
    -> names::NameOperation {
  auto operation = names::NameOperation::acquire(connection, input.name);
  REQUIRE(operation);
  return std::move(*operation);
}
auto transaction(names::NameOperation& operation) -> claims::Transaction {
  auto tx = claims::Transaction::acquire(
      operation, std::chrono::steady_clock::now() + 10s);
  REQUIRE(tx);
  return std::move(*tx);
}
auto receipt(const Input& input, const packages::AuthorizedHttpSubmission& auth,
             std::string id) -> claims::Receipt {
  return {{},
          std::move(id),
          input.name,
          input.version,
          "user",
          std::string(64, 'a'),
          std::string(64, 'b'),
          auth.declarations_sha256(),
          std::string(64, 'c'),
          input.declaration,
          "kernel-test-slot",
          "server/http.js"};
}
auto artifact(names::NameOperation& operation, const claims::Receipt& receipt)
    -> void {
  const generation::GenerationReceipt record{
      {receipt.package_id, receipt.name, receipt.version, 1},
      receipt.artifact_digest,
      receipt.registered_declarations_digest,
      1,
      3,
      64};
  REQUIRE(generation::insert_matching_receipt(operation, record));
}
} // namespace

TEST_CASE(
    "HTTP claim schema checks independent definitions and creates no authority",
    "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  auto outside = claims::ensure_schema(*f.connection);
  REQUIRE_FALSE(outside);
  REQUIRE(outside.error() == claims::Error::TRANSACTION_REQUIRED);
  command(*f.connection, "BEGIN");
  REQUIRE(claims::ensure_schema(*f.connection));
  REQUIRE(claims::ensure_schema(*f.connection));
  REQUIRE(
      scalar(*f.connection,
             "SELECT count(*)::text FROM plinth.package_http_claim_receipts") ==
      "0");
  REQUIRE(scalar(*f.connection,
                 "SELECT count(*)::text FROM "
                 "plinth.package_http_claim_reservations") == "0");
  REQUIRE(
      scalar(
          *f.connection,
          "SELECT count(*)::text FROM plinth.package_http_adapter_bindings") ==
      "0");
  command(*f.connection, "ALTER TABLE plinth.package_http_claim_receipts ALTER "
                         "COLUMN name DROP NOT NULL");
  auto incompatible = claims::ensure_schema(*f.connection);
  REQUIRE_FALSE(incompatible);
  REQUIRE(incompatible.error() == claims::Error::SCHEMA_MISMATCH);
  command(*f.connection, "ROLLBACK");
  command(*f.connection, "BEGIN");
  REQUIRE(claims::ensure_schema(*f.connection));
  command(*f.connection, "COMMIT");
}

TEST_CASE(
    "HTTP claims require the actual name transaction isolation and xact lock",
    "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"claimfixture", "/dav/*"};
  auto operation = acquire(f.connection, input);
  auto absent = claims::Transaction::acquire(
      operation, std::chrono::steady_clock::now() + 10s);
  REQUIRE_FALSE(absent);
  REQUIRE(absent.error() == claims::Error::TRANSACTION_REQUIRED);
  command(*f.connection, "BEGIN ISOLATION LEVEL REPEATABLE READ");
  auto stale_snapshot = claims::Transaction::acquire(
      operation, std::chrono::steady_clock::now() + 10s);
  REQUIRE_FALSE(stale_snapshot);
  REQUIRE(stale_snapshot.error() == claims::Error::TRANSACTION_REQUIRED);
  command(*f.connection, "ROLLBACK");
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  REQUIRE(tx.assert_current());
  command(*f.connection, "COMMIT");
  REQUIRE_FALSE(tx.assert_current());
  command(*f.connection, "BEGIN");
  auto stale = tx.assert_current();
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error() == claims::Error::STALE_TRANSACTION);
  auto current = transaction(operation);
  REQUIRE(current.assert_current());
  command(*f.connection, "ROLLBACK");
  std::stop_source source;
  source.request_stop();
  auto cancelled = claims::Transaction::acquire(
      operation, std::chrono::steady_clock::now() + 10s, source.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error() == claims::Error::CANCELLED);
  auto expired =
      claims::Transaction::acquire(operation, std::chrono::steady_clock::now());
  REQUIRE_FALSE(expired);
  REQUIRE(expired.error() == claims::Error::DEADLINE);
  REQUIRE(operation.release_checked());
  command(*f.connection, "BEGIN");
  auto unlocked = claims::Transaction::acquire(
      operation, std::chrono::steady_clock::now() + 10s);
  REQUIRE_FALSE(unlocked);
  REQUIRE(unlocked.error() == claims::Error::LOCK_NOT_OWNED);
  command(*f.connection, "ROLLBACK");
}

TEST_CASE("HTTP provisional claims remain caller transaction owned and never "
          "backfill ACTIVE",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"claimfixture", "/dav/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth);
  auto operation = acquire(f.connection, input);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  auto reserved = tx.reserve(auth, id, {});
  REQUIRE(reserved);
  REQUIRE_FALSE(reserved->generation_receipt_id);
  REQUIRE(scalar(*f.observer->conn,
                 "SELECT count(*)::text FROM "
                 "plinth.package_http_claim_reservations") == "0");
  command(*f.connection, "ROLLBACK");
  command(*f.connection, "BEGIN");
  auto next = transaction(operation);
  auto missing = next.read_reservation(id);
  REQUIRE(missing);
  REQUIRE_FALSE(missing->has_value());
  REQUIRE(next.reserve(auth, id, {}));
  command(*f.connection, "COMMIT");
  REQUIRE(scalar(*f.observer->conn,
                 "SELECT count(*)::text FROM "
                 "plinth.package_http_claim_reservations") == "1");
  command(*f.connection, "BEGIN");
  auto read = transaction(operation);
  auto stored = read.read_reservation(id);
  REQUIRE(stored);
  REQUIRE(stored->has_value());
  REQUIRE((**stored).http_prefixes == input.declaration.http_prefixes);
  REQUIRE(read.reserve(auth, id, {}) == **stored);
  command(*f.connection, "ROLLBACK");
  Input legacy{"legacyfixture", "/legacy/*"};
  auto legacy_auth = legacy.authorize(f.actor);
  auto legacy_id = f.seed(legacy, legacy_auth, "ACTIVE");
  REQUIRE(operation.release_checked());
  auto legacy_op = acquire(f.connection, legacy);
  command(*f.connection, "BEGIN");
  auto legacy_tx = transaction(legacy_op);
  auto denied = legacy_tx.reserve(legacy_auth, legacy_id, {});
  REQUIRE_FALSE(denied);
  REQUIRE(denied.error() == claims::Error::GENERATION_MISMATCH);
  auto no_receipt = legacy_tx.read_receipt(legacy_id);
  REQUIRE(no_receipt);
  REQUIRE_FALSE(no_receipt->has_value());
  command(*f.connection, "ROLLBACK");
}

TEST_CASE("HTTP receipts bind artifacts and exact retry retains the first UUID",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"claimfixture", "/dav/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth, "ACTIVATING");
  auto operation = acquire(f.connection, input);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  REQUIRE(tx.reserve(auth, id, {}));
  auto proposed = receipt(input, auth, id);
  auto absent = tx.insert_or_match_receipt(auth, proposed);
  REQUIRE_FALSE(absent);
  REQUIRE(absent.error() == claims::Error::GENERATION_MISMATCH);
  artifact(operation, proposed);
  auto inserted = tx.insert_or_match_receipt(auth, proposed);
  REQUIRE(inserted);
  REQUIRE(plinth::extension_http::valid_uuid(inserted->generation_receipt_id));
  proposed.generation_receipt_id = OTHER;
  auto retried = tx.insert_or_match_receipt(auth, proposed);
  REQUIRE(retried);
  REQUIRE(*retried == *inserted);
  auto changed = proposed;
  changed.adapter_binding_key = "changed-slot";
  auto conflict = tx.insert_or_match_receipt(auth, changed);
  REQUIRE_FALSE(conflict);
  REQUIRE(conflict.error() == claims::Error::CONFLICT);
  changed = proposed;
  changed.artifact_digest = std::string(64, 'd');
  conflict = tx.insert_or_match_receipt(auth, changed);
  REQUIRE_FALSE(conflict);
  REQUIRE(conflict.error() == claims::Error::CONFLICT);
  auto attached = tx.attach_receipt(auth, id, inserted->generation_receipt_id);
  REQUIRE(attached);
  REQUIRE(attached->generation_receipt_id == inserted->generation_receipt_id);
  REQUIRE(tx.attach_receipt(auth, id, inserted->generation_receipt_id) ==
          *attached);
  REQUIRE_FALSE(tx.attach_receipt(auth, id, OTHER));
  command(*f.connection, "COMMIT");
  command(*f.connection, "BEGIN");
  auto reread = transaction(operation);
  auto stored = reread.read_receipt(id);
  REQUIRE(stored);
  REQUIRE(stored->has_value());
  REQUIRE(**stored == *inserted);
  command(*f.connection, "ROLLBACK");
}

TEST_CASE("HTTP binding tuples are immutable values and cannot resurrect "
          "retired epochs",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"claimfixture", "/dav/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth, "ACTIVATING");
  auto operation = acquire(f.connection, input);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  REQUIRE(tx.reserve(auth, id, {}));
  auto proposed = receipt(input, auth, id);
  artifact(operation, proposed);
  auto created = tx.insert_or_match_receipt(auth, proposed);
  REQUIRE(created);
  REQUIRE(tx.attach_receipt(auth, id, created->generation_receipt_id));
  claims::AdapterBinding binding{EPOCH, created->generation_receipt_id, OTHER,
                                 EPOCH, std::string(64, 'c'),           false};
  REQUIRE(tx.insert_or_match_binding(auth, binding) == binding);
  REQUIRE(tx.insert_or_match_binding(auth, binding) == binding);
  auto changed = binding;
  changed.owner_epoch = EPOCH;
  auto conflict = tx.insert_or_match_binding(auth, changed);
  REQUIRE_FALSE(conflict);
  REQUIRE(conflict.error() == claims::Error::CONFLICT);
  changed = binding;
  changed.adapter_descriptor_digest = std::string(64, 'd');
  REQUIRE_FALSE(tx.insert_or_match_binding(auth, changed));
  // Stored metadata fixture only: this SQL marker is NOT a physical-retirement
  // proof.
  command(*f.connection,
          "UPDATE plinth.package_http_adapter_bindings SET retired_at=now()");
  auto retired = tx.read_binding(EPOCH);
  REQUIRE(retired);
  REQUIRE(retired->has_value());
  REQUIRE((**retired).retired);
  REQUIRE_FALSE(tx.insert_or_match_binding(auth, binding));
  command(*f.connection, "COMMIT");
}

TEST_CASE("HTTP two-session xact contention and fresh snapshot prevent "
          "conflicting commits",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input first{"firstfixture", "/shared/*"};
  Input second{"secondfixture", "/shared/file"};
  auto a = first.authorize(f.actor);
  auto b = second.authorize(f.actor);
  auto aid = f.seed(first, a);
  auto bid = f.seed(second, b);
  auto other = connect(f.config.db);
  auto own_a = acquire(f.connection, first);
  auto own_b = acquire(other, second);
  command(*f.connection, "BEGIN");
  command(*other, "BEGIN");
  REQUIRE(scalar(*other, "SELECT count(*)::text FROM "
                         "plinth.package_http_claim_reservations") == "0");
  auto held = transaction(own_a);
  REQUIRE(held.reserve(a, aid, {}));
  // These are actual overlapping PostgreSQL transactions/sessions, not a mock
  // lock.
  auto busy = claims::Transaction::acquire(
      own_b, std::chrono::steady_clock::now() + 10s);
  REQUIRE_FALSE(busy);
  REQUIRE(busy.error() == claims::Error::BUSY);
  command(*f.connection, "COMMIT");
  auto fresh = transaction(own_b);
  auto conflicts = fresh.conflicts(b, bid, {});
  REQUIRE(conflicts);
  REQUIRE(*conflicts == std::vector<std::string>{"/shared/file"});
  auto refused = fresh.reserve(b, bid, {});
  REQUIRE_FALSE(refused);
  REQUIRE(refused.error() == claims::Error::CONFLICT);
  REQUIRE(PQtransactionStatus(other.get()) == PQTRANS_INTRANS);
  command(*other, "COMMIT");
  REQUIRE(scalar(*f.observer->conn,
                 "SELECT count(*)::text FROM "
                 "plinth.package_http_claim_reservations") == "1");
}

TEST_CASE("HTTP disjoint near-miss claims commit and conflict scan crosses "
          "page boundary",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  for (int index = 0; index < 33; ++index) {
    Input input{"pagefixture" + std::to_string(index),
                "/space" + std::to_string(index) + "/*"};
    auto auth = input.authorize(f.actor);
    auto id = f.seed(input, auth);
    auto operation = acquire(f.connection, input);
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    REQUIRE(tx.reserve(auth, id, {}));
    command(*f.connection, "COMMIT");
  }
  auto last = scalar(
      *f.observer->conn,
      "SELECT http_prefixes[1] FROM plinth.package_http_claim_reservations "
      "ORDER BY package_id DESC LIMIT 1");
  REQUIRE(last.ends_with("/*"));
  last.pop_back();
  last += "file";
  Input input{"finalfixture", last};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth);
  auto operation = acquire(f.connection, input);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  auto collision = tx.conflicts(auth, id, {});
  REQUIRE(collision);
  REQUIRE(*collision == std::vector<std::string>{last});
  REQUIRE_FALSE(tx.reserve(auth, id, {}));
  command(*f.connection, "ROLLBACK");
  REQUIRE(scalar(*f.observer->conn,
                 "SELECT count(*)::text FROM "
                 "plinth.package_http_claim_reservations") == "33");
}

TEST_CASE("HTTP frontend conflict scan retains exact states and fails unknown "
          "ownership closed",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"claimfixture", "/app/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth);
  Input front{"frontfixture", "/frontend-test/*"};
  auto front_auth = front.authorize(f.actor);
  auto front_id = f.seed(front, front_auth, "UPLOADING", "/app/");
  auto operation = acquire(f.connection, input);
  for (auto state :
       {"UPLOADING", "VALIDATING", "MIGRATING", "REGISTERING", "EXTRACTING",
        "ACTIVATING", "ACTIVE", "ACTIVE_FLAGGED", "DISABLED", "UNINSTALLING",
        "INSTALL_FAILED", "SUPERSEDED"}) {
    auto update = f.observer->exec_params(
        "UPDATE plinth.packages SET state=$2 WHERE id=$1::uuid",
        {front_id, state});
    REQUIRE(PQresultStatus(update.get()) == PGRES_COMMAND_OK);
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    auto conflicts = tx.conflicts(auth, id, {});
    REQUIRE(conflicts);
    if (std::string_view{state} == "INSTALL_FAILED" ||
        std::string_view{state} == "SUPERSEDED")
      REQUIRE(conflicts->empty());
    else
      REQUIRE(*conflicts == std::vector<std::string>{"/app/*"});
    command(*f.connection, "ROLLBACK");
  }
  auto near = f.observer->exec_params(
      "UPDATE plinth.packages SET state='ACTIVE',frontend_mount='/appx' WHERE "
      "id=$1::uuid",
      {front_id});
  REQUIRE(PQresultStatus(near.get()) == PGRES_COMMAND_OK);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  auto clear = tx.conflicts(auth, id, {});
  REQUIRE(clear);
  REQUIRE(clear->empty());
  const std::array<claims::MountedFrontendRoot, 1> mounted{
      {{"/app", std::nullopt}}};
  REQUIRE(tx.conflicts(auth, id, mounted) ==
          std::vector<std::string>{"/app/*"});
  command(*f.connection, "ROLLBACK");
  // Benign legacy root; it remains installed. New allocation does not assume
  // regex punctuation is literal and does not change old mount validation.
  auto legacy =
      f.observer->exec_params("UPDATE plinth.packages SET "
                              "frontend_mount='/legacy.name' WHERE id=$1::uuid",
                              {front_id});
  REQUIRE(PQresultStatus(legacy.get()) == PGRES_COMMAND_OK);
  command(*f.connection, "BEGIN");
  auto unknown = transaction(operation);
  auto unavailable = unknown.reserve(auth, id, {});
  REQUIRE_FALSE(unavailable);
  REQUIRE(unavailable.error() == claims::Error::UNAVAILABLE);
  REQUIRE(scalar(*f.connection,
                 "SELECT count(*)::text FROM plinth.packages WHERE "
                 "state='ACTIVE' AND frontend_mount='/legacy.name'") == "1");
  command(*f.connection, "ROLLBACK");
}

TEST_CASE("HTTP held records prevent ordinary cascade deletion without a "
          "retirement proof",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"claimfixture", "/held/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth);
  auto operation = acquire(f.connection, input);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  REQUIRE(tx.reserve(auth, id, {}));
  command(*f.connection, "COMMIT");
  command(*f.connection, "BEGIN");
  const std::array<const char*, 1> parameters{id.c_str()};
  const PgResult deleted{plinth::db::exec_params(
                             f.connection.get(),
                             "DELETE FROM plinth.packages WHERE id=$1::uuid", 1,
                             nullptr, parameters.data(), nullptr, nullptr, 0),
                         PQclear};
  REQUIRE(deleted != nullptr);
  REQUIRE(PQresultStatus(deleted.get()) == PGRES_FATAL_ERROR);
  const char* code = PQresultErrorField(deleted.get(), PG_DIAG_SQLSTATE);
  REQUIRE(code != nullptr);
  REQUIRE(std::string{code} == "23503");
  command(*f.connection, "ROLLBACK");
  REQUIRE(scalar(*f.observer->conn,
                 "SELECT count(*)::text FROM "
                 "plinth.package_http_claim_reservations") == "1");
}

TEST_CASE("HTTP upgrade exemption is only the exact authorized predecessor and "
          "never a fixed root",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  SECTION(
      "old and candidate union is retained but fixed mounted roots still win") {
    Input old{"upgradefixture", "/upgrade/*"};
    auto old_auth = old.authorize(f.actor);
    auto old_id = f.seed(old, old_auth);
    {
      auto operation = acquire(f.connection, old);
      command(*f.connection, "BEGIN");
      auto tx = transaction(operation);
      REQUIRE(tx.reserve(old_auth, old_id, {}));
      command(*f.connection, "COMMIT");
    }
    auto active = f.observer->exec_params(
        "UPDATE plinth.packages SET state='ACTIVE' WHERE id=$1::uuid",
        {old_id});
    REQUIRE(PQresultStatus(active.get()) == PGRES_COMMAND_OK);
    Input candidate{"upgradefixture", "/upgrade/file", "2.0.0"};
    auto auth = candidate.authorize(
        f.actor, packages::HttpInstallOperation::UPGRADE, old_id);
    auto id = f.seed(candidate, auth);
    auto operation = acquire(f.connection, candidate);
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    auto no_conflict = tx.conflicts(auth, id, {});
    REQUIRE(no_conflict);
    REQUIRE(no_conflict->empty());
    const std::array<claims::MountedFrontendRoot, 1> fixed{
        {{"/upgrade", std::nullopt}}};
    REQUIRE(tx.conflicts(auth, id, fixed) ==
            std::vector<std::string>{"/upgrade/file"});
    REQUIRE_FALSE(tx.reserve(auth, id, fixed));
    REQUIRE(tx.reserve(auth, id, {}));
    command(*f.connection, "COMMIT");
    REQUIRE(scalar(*f.observer->conn,
                   "SELECT count(*)::text FROM "
                   "plinth.package_http_claim_reservations") == "2");
  }
  SECTION("another retained same-name history row is not the predecessor") {
    Input history{"upgradefixture", "/history/*", "0.5.0"};
    auto historical = history.authorize(f.actor);
    auto historical_id = f.seed(history, historical);
    {
      auto operation = acquire(f.connection, history);
      command(*f.connection, "BEGIN");
      auto tx = transaction(operation);
      REQUIRE(tx.reserve(historical, historical_id, {}));
      command(*f.connection, "COMMIT");
    }
    auto failed = f.observer->exec_params(
        "UPDATE plinth.packages SET state='INSTALL_FAILED' WHERE id=$1::uuid",
        {historical_id});
    REQUIRE(PQresultStatus(failed.get()) == PGRES_COMMAND_OK);
    Input old{"upgradefixture", "/old/*"};
    auto old_auth = old.authorize(f.actor);
    auto old_id = f.seed(old, old_auth);
    {
      auto operation = acquire(f.connection, old);
      command(*f.connection, "BEGIN");
      auto tx = transaction(operation);
      REQUIRE(tx.reserve(old_auth, old_id, {}));
      command(*f.connection, "COMMIT");
    }
    auto active = f.observer->exec_params(
        "UPDATE plinth.packages SET state='ACTIVE' WHERE id=$1::uuid",
        {old_id});
    REQUIRE(PQresultStatus(active.get()) == PGRES_COMMAND_OK);
    Input candidate{"upgradefixture", "/history/file", "2.0.0"};
    auto auth = candidate.authorize(
        f.actor, packages::HttpInstallOperation::UPGRADE, old_id);
    auto id = f.seed(candidate, auth);
    auto operation = acquire(f.connection, candidate);
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    REQUIRE(tx.conflicts(auth, id, {}) ==
            std::vector<std::string>{"/history/file"});
    REQUIRE_FALSE(tx.reserve(auth, id, {}));
    command(*f.connection, "ROLLBACK");
  }
  SECTION("candidate own frontend is independently reserved") {
    Input candidate{"ownfixture", "/own/*", "1.0.0", "/own"};
    auto auth = candidate.authorize(f.actor);
    auto id = f.seed(candidate, auth, "VALIDATING", "/own");
    auto operation = acquire(f.connection, candidate);
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    REQUIRE(tx.conflicts(auth, id, {}) == std::vector<std::string>{"/own/*"});
    REQUIRE_FALSE(tx.reserve(auth, id, {}));
    command(*f.connection, "ROLLBACK");
  }
}

TEST_CASE("HTTP attached receipt UUID cannot cross package ownership even "
          "through a direct FK write",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input first{"firstfixture", "/first/*"};
  Input second{"secondfixture", "/second/*"};
  auto a = first.authorize(f.actor);
  auto b = second.authorize(f.actor);
  auto aid = f.seed(first, a, "ACTIVATING");
  auto bid = f.seed(second, b, "ACTIVATING");
  std::array<std::string, 2> receipt_ids;
  for (std::size_t index = 0; index < 2; ++index) {
    const auto& input = index == 0 ? first : second;
    const auto& auth = index == 0 ? a : b;
    const auto& id = index == 0 ? aid : bid;
    auto operation = acquire(f.connection, input);
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    REQUIRE(tx.reserve(auth, id, {}));
    auto proposed = receipt(input, auth, id);
    artifact(operation, proposed);
    auto created = tx.insert_or_match_receipt(auth, proposed);
    REQUIRE(created);
    receipt_ids[index] = created->generation_receipt_id;
    REQUIRE(tx.attach_receipt(auth, id, receipt_ids[index]));
    command(*f.connection, "COMMIT");
  }
  auto operation = acquire(f.connection, first);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  REQUIRE_FALSE(tx.attach_receipt(a, aid, receipt_ids[1]));
  const std::array<const char*, 2> parameters{aid.c_str(),
                                              receipt_ids[1].c_str()};
  const PgResult crossed{
      plinth::db::exec_params(
          f.connection.get(),
          "UPDATE plinth.package_http_claim_reservations SET "
          "generation_receipt_id=$2::uuid WHERE package_id=$1::uuid",
          2, nullptr, parameters.data(), nullptr, nullptr, 0),
      PQclear};
  REQUIRE(crossed != nullptr);
  REQUIRE(PQresultStatus(crossed.get()) == PGRES_FATAL_ERROR);
  const char* code = PQresultErrorField(crossed.get(), PG_DIAG_SQLSTATE);
  REQUIRE(code != nullptr);
  REQUIRE(std::string{code} == "23503");
  command(*f.connection, "ROLLBACK");
  command(*f.connection, "BEGIN");
  auto fresh = transaction(operation);
  auto unchanged = fresh.read_reservation(aid);
  REQUIRE(unchanged);
  REQUIRE(unchanged->has_value());
  REQUIRE((**unchanged).generation_receipt_id == receipt_ids[0]);
  command(*f.connection, "ROLLBACK");
}

TEST_CASE("HTTP committed reader requires fresh IDLE ACTIVE same-name metadata",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"readerfixture", "/reader/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth, "ACTIVATING");
  auto operation = acquire(f.connection, input);
  const generation::GenerationKey key{id, input.name, input.version, 1};
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  REQUIRE(tx.reserve(auth, id, {}));
  auto proposed = receipt(input, auth, id);
  artifact(operation, proposed);
  auto created = tx.insert_or_match_receipt(auth, proposed);
  REQUIRE(created);
  REQUIRE(tx.attach_receipt(auth, id, created->generation_receipt_id));
  command(*f.connection, "UPDATE plinth.packages SET state='ACTIVE'");
  auto uncommitted = claims::read_committed_receipt(operation, key, deadline);
  REQUIRE_FALSE(uncommitted);
  REQUIRE(uncommitted.error() == claims::Error::TRANSACTION_REQUIRED);
  REQUIRE(PQtransactionStatus(f.connection.get()) == PQTRANS_INTRANS);
  command(*f.connection, "COMMIT");
  auto observed = claims::read_committed_receipt(operation, key, deadline);
  REQUIRE(observed);
  REQUIRE(observed->has_value());
  REQUIRE(**observed == *created);
  REQUIRE(PQtransactionStatus(f.connection.get()) == PQTRANS_IDLE);
  command(*f.connection, "UPDATE plinth.packages SET state='ACTIVE_FLAGGED'");
  REQUIRE(claims::read_committed_receipt(operation, key, deadline) == observed);
  auto wrong = key;
  wrong.version = "2.0.0";
  REQUIRE_FALSE(claims::read_committed_receipt(operation, wrong, deadline));
  std::stop_source source;
  source.request_stop();
  auto cancelled = claims::read_committed_receipt(operation, key, deadline,
                                                  source.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error() == claims::Error::CANCELLED);
  auto expired = claims::read_committed_receipt(
      operation, key, std::chrono::steady_clock::now());
  REQUIRE_FALSE(expired);
  REQUIRE(expired.error() == claims::Error::DEADLINE);
  REQUIRE(operation.release_checked());
  auto unlocked = claims::read_committed_receipt(operation, key, deadline);
  REQUIRE_FALSE(unlocked);
  REQUIRE(unlocked.error() == claims::Error::LOCK_NOT_OWNED);
}

TEST_CASE("HTTP committed reader distinguishes inert legacy from partial "
          "reservations",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"legacyreader", "/legacy-reader/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth, "ACTIVATING");
  auto operation = acquire(f.connection, input);
  const generation::GenerationKey key{id, input.name, input.version, 1};
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  SECTION(
      "no HTTP records stays inert even with malformed old unknown fields") {
    auto legacy = nlohmann::ordered_json::parse(input.raw);
    legacy["http_prefixes"] =
        nlohmann::ordered_json::object({{"old-unknown", true}});
    legacy["handler_mode"] = 99;
    const auto raw = legacy.dump();
    const auto hash = packages::http_install_zip_sha256(
        std::as_bytes(std::span<const char>{raw.data(), raw.size()}));
    const auto changed =
        f.observer->exec_params("UPDATE plinth.packages SET "
                                "state='ACTIVE',manifest_json=$2::jsonb,"
                                "manifest_checksum=$3 WHERE id=$1::uuid",
                                {id, raw, hash});
    REQUIRE(PQresultStatus(changed.get()) == PGRES_COMMAND_OK);
    auto inert = claims::read_committed_receipt(operation, key, deadline);
    REQUIRE(inert);
    REQUIRE_FALSE(inert->has_value());
    // An existing ordinary artifact receipt still cannot backfill HTTP
    // authority.
    command(*f.connection, "BEGIN");
    artifact(operation, receipt(input, auth, id));
    command(*f.connection, "COMMIT");
    auto still_inert = claims::read_committed_receipt(operation, key, deadline);
    REQUIRE(still_inert);
    REQUIRE_FALSE(still_inert->has_value());
    REQUIRE(
        scalar(
            *f.connection,
            "SELECT count(*)::text FROM plinth.package_http_claim_receipts") ==
        "0");
    REQUIRE(scalar(*f.connection,
                   "SELECT count(*)::text FROM "
                   "plinth.package_http_claim_reservations") == "0");
    REQUIRE(scalar(*f.connection,
                   "SELECT manifest_json->>'handler_mode' FROM plinth.packages "
                   "WHERE name='legacyreader'") == "99");
  }
  SECTION("provisional reservation is unavailable rather than legacy absence") {
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    REQUIRE(tx.reserve(auth, id, {}));
    command(*f.connection, "UPDATE plinth.packages SET state='ACTIVE'");
    command(*f.connection, "COMMIT");
    auto partial = claims::read_committed_receipt(operation, key, deadline);
    REQUIRE_FALSE(partial);
    REQUIRE(partial.error() == claims::Error::UNAVAILABLE);
  }
  SECTION("receipt without attached reservation is not publication metadata") {
    command(*f.connection, "BEGIN");
    auto tx = transaction(operation);
    REQUIRE(tx.reserve(auth, id, {}));
    auto proposed = receipt(input, auth, id);
    artifact(operation, proposed);
    REQUIRE(tx.insert_or_match_receipt(auth, proposed));
    command(*f.connection, "UPDATE plinth.packages SET state='ACTIVE'");
    command(*f.connection, "COMMIT");
    auto partial = claims::read_committed_receipt(operation, key, deadline);
    REQUIRE_FALSE(partial);
    REQUIRE(partial.error() == claims::Error::UNAVAILABLE);
  }
}

TEST_CASE("HTTP committed reader rejects committed identity attachment and "
          "state mismatches",
          "[packages][http_claims][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture f;
  Input input{"mismatchreader", "/reader/*"};
  auto auth = input.authorize(f.actor);
  auto id = f.seed(input, auth, "ACTIVATING");
  auto operation = acquire(f.connection, input);
  command(*f.connection, "BEGIN");
  auto tx = transaction(operation);
  REQUIRE(tx.reserve(auth, id, {}));
  auto proposed = receipt(input, auth, id);
  artifact(operation, proposed);
  auto created = tx.insert_or_match_receipt(auth, proposed);
  REQUIRE(created);
  REQUIRE(tx.attach_receipt(auth, id, created->generation_receipt_id));
  command(*f.connection, "UPDATE plinth.packages SET state='ACTIVE'");
  command(*f.connection, "COMMIT");
  const generation::GenerationKey key{id, input.name, input.version, 1};
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  REQUIRE(claims::read_committed_receipt(operation, key, deadline));
  // Committed row fixtures test pure read consistency; none is a successful
  // physical retirement, lifecycle transition, dispatch or publication proof.
  SECTION("artifact identity differs") {
    command(*f.connection, "UPDATE plinth.package_artifact_receipts SET "
                           "artifact_digest=repeat('d',64)");
  }
  SECTION("registered declaration identity differs") {
    command(*f.connection, "UPDATE plinth.package_artifact_receipts SET "
                           "declarations_digest=repeat('d',64)");
  }
  SECTION("reservation prefix differs") {
    command(*f.connection, "UPDATE plinth.package_http_claim_reservations SET "
                           "http_prefixes=ARRAY['/different/*']");
  }
  SECTION("reservation declaration digest differs") {
    command(*f.connection, "UPDATE plinth.package_http_claim_reservations SET "
                           "http_declarations_digest=repeat('d',64)");
  }
  SECTION("retiring attachment cannot be republished") {
    command(
        *f.connection,
        "UPDATE plinth.package_http_claim_reservations SET phase='RETIRING'");
  }
  SECTION("missing reservation remains inconsistent modern metadata") {
    command(*f.connection,
            "DELETE FROM plinth.package_http_claim_reservations");
  }
  SECTION("stored provenance differs") {
    command(*f.connection, "UPDATE plinth.packages SET provenance='bundled'");
  }
  SECTION("disabled row cannot publish") {
    command(*f.connection, "UPDATE plinth.packages SET state='DISABLED'");
  }
  SECTION("replaced row cannot publish") {
    command(*f.connection, "UPDATE plinth.packages SET state='SUPERSEDED'");
    Input successor{"mismatchreader", "/successor/*", "2.0.0"};
    auto next = successor.authorize(
        f.actor, packages::HttpInstallOperation::UPGRADE, id);
    static_cast<void>(f.seed(successor, next, "ACTIVE"));
  }
  auto refused = claims::read_committed_receipt(operation, key, deadline);
  REQUIRE_FALSE(refused);
  REQUIRE(PQtransactionStatus(f.connection.get()) == PQTRANS_IDLE);
}
