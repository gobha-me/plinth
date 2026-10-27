#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/packages/http_generation_stage.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/installed_generation_registry.hpp"
#include "kernel/packages/migrations_internal.hpp"
#include "kernel/rbac/enforcement.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace generation = plinth::packages::generation;
namespace names = plinth::packages::name_operation;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Error = generation::InstalledGenerationError;

template <typename Value>
auto require_error(const std::expected<Value, Error>& result, Error error)
    -> void {
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == error);
}

struct Tree {
  fs::path root;
  Tree() {
    std::array<char, 80> pattern{};
    std::ranges::copy(
        std::string_view{"/tmp/plinth-installed-generation.XXXXXX"},
        pattern.begin());
    const char* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("generation test tree creation");
    root = created;
  }
  ~Tree() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  Tree(const Tree&) = delete;
  auto operator=(const Tree&) -> Tree& = delete;
};

constexpr auto MANIFEST = R"({"name":"registryfixture","version":"1.0.0",
"description":"owned registry fixture","author":"plinth","license":"MIT",
"entry_point":"server/main.js"})";

struct Fixture {
  Tree tree;
  plinth::Config config{plinth::ws_test::test_config()};
  std::shared_ptr<PGconn> connection;
  generation::GenerationKey key{.package_id = {},
                                .name = "registryfixture",
                                .version = "1.0.0",
                                .profile = 1};
  fs::path version_root{tree.root / "extensions" / key.name / key.version};
  explicit Fixture(std::string raw_manifest = MANIFEST) {
    plinth::ws_test::reset_schema(config.db);
    connection = std::shared_ptr<PGconn>{
        PQconnectdb(plinth::db::connection_info(config.db).c_str()), PQfinish};
    REQUIRE(connection);
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
    fs::create_directories(version_root / "server");
    fs::create_directories(version_root / "client");
    write("manifest.json", raw_manifest);
    write("panels.json", "{\"panels\":[]}");
    write("capabilities.json", "{\"provides\":[],\"requires\":[]}");
    write("server/main.js", "export default {};\n");
    write("client/value.txt", "original\n");
    const auto checksum = plinth::packages::detail::sha256_hex(raw_manifest);
    const std::array<const char*, 2> parameters{raw_manifest.c_str(),
                                                checksum.c_str()};
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexecParams(
            connection.get(),
            "INSERT INTO plinth.packages(name,version,state,provenance,"
            "manifest_json,entry_point,manifest_checksum) VALUES"
            "('registryfixture','1.0.0','ACTIVATING','user',$1::jsonb,"
            "'server/main.js',$2) RETURNING id::text",
            2, nullptr, parameters.data(), nullptr, nullptr, 0),
        PQclear};
    REQUIRE(result);
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    key.package_id = PQgetvalue(result.get(), 0, 0);
  }
  auto write(std::string_view member, std::string_view bytes) -> void {
    std::ofstream stream(version_root / member,
                         std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
  }
  auto command(const char* sql) -> void {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql), PQclear};
    REQUIRE(result);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto lock() -> names::NameOperation {
    auto operation = names::NameOperation::acquire(connection, key.name);
    REQUIRE(operation);
    return std::move(*operation);
  }
  auto deadline() const -> generation::InstalledGenerationRegistry::Deadline {
    return std::chrono::steady_clock::now() + 10s;
  }
  auto activate() -> void {
    command("UPDATE plinth.packages SET state='ACTIVE' "
            "WHERE name='registryfixture'");
  }
};

constexpr auto HTTP_MANIFEST = R"({"name":"registryfixture","version":"1.0.0",
"description":"owned registry fixture","author":"plinth","license":"MIT",
"entry_point":"server/main.js","http_prefixes":["/capture/*"],"handler_mode":"quickjs"})";

auto authorize_http(std::string_view raw)
    -> plinth::packages::AuthorizedHttpSubmission {
  // Trusted request-attribute fixture through the production proof factory.
  // No actual ZIP install or authentication filter-chain claim is made here.
  const std::array zip{std::byte{'P'}, std::byte{'K'}, std::byte{3},
                       std::byte{4}};
  auto request = drogon::HttpRequest::newHttpRequest();
  request->attributes()->insert(
      plinth::auth::ATTR_USER_ID,
      std::string{"11111111-1111-1111-1111-111111111111"});
  request->attributes()->insert(plinth::auth::ATTR_AUTH_TYPE,
                                std::string{"session"});
  request->attributes()->insert(plinth::auth::ATTR_CREDENTIAL_SOURCE,
                                std::string{"cookie"});
  request->attributes()->insert(plinth::rbac::ATTR_EFFECTIVE_RULES,
                                std::vector<std::string>{"kernel.admin"});
  request->attributes()->insert(plinth::rbac::ATTR_PERMISSION_GRANTED, true);
  auto declaration = plinth::packages::HttpDeclaration::parse(raw);
  REQUIRE(declaration.value);
  const plinth::packages::HttpInstallAuthorizationRequest input{
      zip,
      raw,
      *declaration.value,
      "registryfixture",
      "1.0.0",
      plinth::packages::HttpInstallOperation::INSTALL,
      {},
      plinth::packages::Provenance::USER};
  auto result =
      plinth::packages::make_http_install_authorizer(request, zip)(input);
  REQUIRE(result);
  return std::move(*result);
}
} // namespace

TEST_CASE("installed generation registry: receipt commit and immutable cache "
          "ownership",
          "[packages][installed_generation_registry][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture fixture;
  generation::InstalledGenerationRegistry registry{fixture.tree.root};
  auto operation = fixture.lock();
  SECTION(
      "stage requires caller transaction and current read requires receipt") {
    require_error(
        registry.stage_locked(operation, fixture.key, fixture.deadline()),
        Error::TRANSACTION_REQUIRED);
    fixture.activate();
    require_error(
        registry.current_locked(operation, fixture.key, fixture.deadline()),
        Error::RECEIPT_REQUIRED);
  }
  SECTION("rollback never publishes an uncommitted generation") {
    fixture.command("BEGIN");
    auto prepared =
        registry.stage_locked(operation, fixture.key, fixture.deadline());
    REQUIRE(prepared);
    require_error(registry.publish_committed_locked(operation, *prepared,
                                                    fixture.deadline()),
                  Error::TRANSACTION_REQUIRED);
    fixture.command("ROLLBACK");
    fixture.activate();
    require_error(registry.publish_committed_locked(operation, *prepared,
                                                    fixture.deadline()),
                  Error::RECEIPT_REQUIRED);
    require_error(
        registry.current_locked(operation, fixture.key, fixture.deadline()),
        Error::RECEIPT_REQUIRED);
  }
  SECTION("acknowledged activation publishes while cold changed bytes refuse") {
    fixture.command("BEGIN");
    auto prepared =
        registry.stage_locked(operation, fixture.key, fixture.deadline());
    REQUIRE(prepared);
    plinth::ws_test::TestPg observer{fixture.config.db};
    auto count = observer.exec(
        "SELECT count(*)::text FROM plinth.package_artifact_receipts");
    REQUIRE(PQresultStatus(count.get()) == PGRES_TUPLES_OK);
    REQUIRE(std::string_view{PQgetvalue(count.get(), 0, 0)} == "0");
    fixture.activate();
    fixture.command("COMMIT");
    REQUIRE(registry.publish_committed_locked(operation, *prepared,
                                              fixture.deadline()));
    auto current =
        registry.current_locked(operation, fixture.key, fixture.deadline());
    REQUIRE(current);
    REQUIRE((*current)->digest() == prepared->receipt().artifact_digest);
    REQUIRE((*current)->find("client/value.txt")->content == "original\n");
    SECTION("warm bytes remain immutable; cold recapture cannot bless changed "
            "bytes") {
      fixture.write("client/value.txt", "modified\n");
      auto warm =
          registry.current_locked(operation, fixture.key, fixture.deadline());
      REQUIRE(warm);
      REQUIRE((*warm)->find("client/value.txt")->content == "original\n");
      registry.clear();
      REQUIRE(registry.source_budget().used() > 0);
      require_error(
          registry.current_locked(operation, fixture.key, fixture.deadline()),
          Error::MISMATCH);
      fixture.write("client/value.txt", "original\n");
      REQUIRE(
          registry.current_locked(operation, fixture.key, fixture.deadline()));
    }
    SECTION("retired registration denies fresh reads without destroying old "
            "bytes") {
      fixture.command("UPDATE plinth.packages SET state='SUPERSEDED' "
                      "WHERE name='registryfixture'");
      registry.retire(fixture.key.package_id);
      require_error(
          registry.current_locked(operation, fixture.key, fixture.deadline()),
          Error::MISMATCH);
      REQUIRE((*current)->find("client/value.txt")->content == "original\n");
    }
    SECTION("foreign budget cannot publish another registry's prepared owner") {
      generation::InstalledGenerationRegistry foreign{fixture.tree.root};
      require_error(foreign.publish_committed_locked(operation, *prepared,
                                                     fixture.deadline()),
                    Error::INVALID);
    }
    SECTION("changed registered authority refuses even warm captured bytes") {
      fixture.command("UPDATE plinth.packages SET manifest_json='{}'::jsonb "
                      "WHERE name='registryfixture'");
      REQUIRE_FALSE(
          registry.current_locked(operation, fixture.key, fixture.deadline()));
    }
  }
  SECTION("expired and canceled operations cannot renew work or publish") {
    std::stop_source stop;
    stop.request_stop();
    const auto before = registry.source_budget().used();
    require_error(registry.current_locked(operation, fixture.key,
                                          fixture.deadline(), stop.get_token()),
                  Error::CANCELLED);
    require_error(
        registry.current_locked(operation, fixture.key,
                                std::chrono::steady_clock::now() - 1ms),
        Error::DEADLINE);
    REQUIRE(registry.source_budget().used() == before);
  }
  SECTION("capture pressure is whole unavailable without inserting a receipt") {
    generation::InstalledGenerationRegistry small{
        fixture.tree.root, generation::Budget{std::size_t{64} * 1024}};
    fixture.command("BEGIN");
    require_error(
        small.stage_locked(operation, fixture.key, fixture.deadline()),
        Error::BUSY);
    auto receipt = generation::read_receipt(operation, fixture.key);
    REQUIRE(receipt);
    REQUIRE_FALSE(receipt->has_value());
    fixture.command("ROLLBACK");
  }
  SECTION("clean codec ineligibility cannot poison the caller activation "
          "transaction") {
    SECTION("symlink is outside the new capture profile") {
      fs::remove(fixture.version_root / "client/value.txt");
      fs::create_symlink("../server/main.js",
                         fixture.version_root / "client/value.txt");
    }
    SECTION("oversized sparse member is refused before reading its payload") {
      fixture.write("client/large.bin", "");
      fs::resize_file(fixture.version_root / "client/large.bin",
                      std::uintmax_t{50} * 1024 * 1024 + 1);
    }
    fixture.command("BEGIN");
    require_error(
        registry.stage_locked(operation, fixture.key, fixture.deadline()),
        Error::PROFILE_UNAVAILABLE);
    REQUIRE(PQstatus(fixture.connection.get()) == CONNECTION_OK);
    REQUIRE(PQtransactionStatus(fixture.connection.get()) == PQTRANS_INTRANS);
    auto receipt = generation::read_receipt(operation, fixture.key);
    REQUIRE(receipt);
    REQUIRE_FALSE(receipt->has_value());
    // Caller, not the codec, decides to complete its otherwise-valid legacy
    // activation. This does not publish typed bytes or fabricate a receipt.
    fixture.activate();
    fixture.command("COMMIT");
    require_error(
        registry.current_locked(operation, fixture.key, fixture.deadline()),
        Error::RECEIPT_REQUIRED);
  }
}

TEST_CASE("HTTP capture binds exact authorized manifest without publishing or "
          "evaluating",
          "[packages][http-generation-stage][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  namespace http = plinth::packages::http_generation;
  Fixture fixture{HTTP_MANIFEST};
  const std::string binary_source{"throw 1;\0tail", 13};
  fixture.write("server/http.js", binary_source);
  generation::InstalledGenerationRegistry registry{fixture.tree.root};
  auto budget = registry.source_budget();
  auto operation = fixture.lock();
  fixture.command("BEGIN");
  auto staged =
      registry.stage_locked(operation, fixture.key, fixture.deadline());
  REQUIRE(staged);
  const auto captured_digest = staged->artifact()->digest();
  auto authorized = authorize_http(HTTP_MANIFEST);
  const auto approved_manifest = authorized.raw_manifest_sha256();
  auto bound = http::prepare_http_capture(std::move(*staged),
                                          std::move(authorized), budget);
  REQUIRE(bound);
  REQUIRE(bound->authorization().raw_manifest_sha256() == approved_manifest);
  REQUIRE(bound->prepared_generation().artifact()->digest() == captured_digest);
  REQUIRE(bound->modules());
  REQUIRE(bound->modules()->entry().bytes() == std::string_view{binary_source});
  fixture.write("server/http.js", "different on disk");
  REQUIRE(bound->modules()->entry().bytes() == std::string_view{binary_source});
  require_error(registry.publish_committed_locked(operation,
                                                  bound->prepared_generation(),
                                                  fixture.deadline()),
                Error::TRANSACTION_REQUIRED);
  plinth::ws_test::TestPg observer{fixture.config.db};
  auto count = observer.exec(
      "SELECT (SELECT count(*) FROM plinth.package_http_claim_receipts),"
      "(SELECT count(*) FROM plinth.package_http_claim_reservations),"
      "(SELECT count(*) FROM plinth.package_artifact_receipts)");
  REQUIRE(PQresultStatus(count.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(count.get()) == 1);
  for (int column = 0; column < 3; ++column)
    REQUIRE(std::string_view{PQgetvalue(count.get(), 0, column)} == "0");
  fixture.command("ROLLBACK");
  fixture.activate();
  require_error(registry.publish_committed_locked(operation,
                                                  bound->prepared_generation(),
                                                  fixture.deadline()),
                Error::RECEIPT_REQUIRED);
  REQUIRE(bound->modules()->entry().bytes() == std::string_view{binary_source});
}

TEST_CASE(
    "HTTP capture refuses changed authorized source entry or control budget",
    "[packages][http-generation-stage][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  namespace http = plinth::packages::http_generation;
  Fixture fixture{HTTP_MANIFEST};
  generation::InstalledGenerationRegistry registry{fixture.tree.root};
  auto budget = registry.source_budget();
  auto operation = fixture.lock();
  auto raw = std::string{HTTP_MANIFEST};
  bool write_entry = true;
  auto expected = http::PreparationError::SOURCE_MISMATCH;
  bool foreign = false;
  bool pressure = false;
  SECTION("different exact manifest bytes") {
    raw.insert(raw.size() - 1, " ");
  }
  SECTION("different declaration") {
    raw.replace(raw.find("/capture/*"), std::string_view{"/capture/*"}.size(),
                "/different/*");
  }
  SECTION("missing fixed entry") {
    write_entry = false;
    expected = http::PreparationError::ENTRY_REQUIRED;
  }
  SECTION("another budget cannot inherit source ownership") {
    foreign = true;
    expected = http::PreparationError::INVALID;
  }
  SECTION("control allocation refused before digest copies") {
    pressure = true;
    expected = http::PreparationError::BUDGET_UNAVAILABLE;
  }
  if (write_entry)
    fixture.write("server/http.js", "export default () => ({});");
  fixture.command("BEGIN");
  auto staged =
      registry.stage_locked(operation, fixture.key, fixture.deadline());
  REQUIRE(staged);
  generation::Budget unrelated;
  std::optional<generation::BudgetTicket> hold;
  if (pressure) {
    hold = budget.try_reserve(budget.capacity() - budget.used() -
                              (std::size_t{128} * 1024 - 1));
    REQUIRE(hold);
  }
  auto bound = http::prepare_http_capture(
      std::move(*staged), authorize_http(raw), foreign ? unrelated : budget);
  REQUIRE_FALSE(bound);
  REQUIRE(bound.error() == expected);
  REQUIRE(PQtransactionStatus(fixture.connection.get()) == PQTRANS_INTRANS);
  fixture.command("ROLLBACK");
}
