#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/handlers.hpp"
#include "kernel/packages/http_install_authorization.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/rbac/enforcement.hpp"
#include "static_validation_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace packages = plinth::packages;
using Code = packages::HttpInstallAuthorizationCode;
constexpr std::string_view ACTOR = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view OTHER_ACTOR = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view PREDECESSOR = "33333333-3333-4333-8333-333333333333";
auto private_rules() -> std::vector<std::string> {
  return {"packages.install", "packages.install.with_http_prefixes"};
}
auto public_rules() -> std::vector<std::string> {
  return {"packages.install", "packages.install.with_http_prefixes",
          "packages.install.with_unauthenticated_prefixes"};
}

static_assert(
    !std::is_default_constructible_v<packages::AuthorizedHttpSubmission>);
static_assert(
    !std::is_copy_constructible_v<packages::AuthorizedHttpSubmission>);
static_assert(std::is_move_constructible_v<packages::AuthorizedHttpSubmission>);
static_assert(
    !std::is_constructible_v<packages::KernelBundledHttpInstallToken,
                             std::span<const std::byte>,
                             packages::HttpInstallOperation, std::string_view>);

// Explicit trusted request-attribute fixture. This exercises the production
// snapshot factory, not a claim that SessionFilter/RbacFilter executed here.
auto actor_request(const std::vector<std::string>& rules,
                   std::string_view actor = ACTOR) -> drogon::HttpRequestPtr {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->attributes()->insert(plinth::auth::ATTR_USER_ID, std::string{actor});
  request->attributes()->insert(plinth::auth::ATTR_AUTH_TYPE,
                                std::string{"session"});
  request->attributes()->insert(plinth::auth::ATTR_CREDENTIAL_SOURCE,
                                std::string{"cookie"});
  request->attributes()->insert(plinth::rbac::ATTR_EFFECTIVE_RULES, rules);
  request->attributes()->insert(plinth::rbac::ATTR_PERMISSION_GRANTED, true);
  return request;
}

struct Input {
  nlohmann::json manifest{{"name", "notes"},
                          {"version", "1.2.3"},
                          {"description", "HTTP authorization fixture"},
                          {"author", "plinth-tests"},
                          {"license", "MIT"},
                          {"entry_point", "server/main.js"},
                          {"http_prefixes", {"/dav/*"}},
                          {"handler_mode", "quickjs"}};
  std::string raw;
  packages::HttpDeclaration declaration;
  // Pure binding bytes; real ZIP/lifecycle behavior is covered by PG cases.
  std::vector<std::byte> zip{std::byte{'P'}, std::byte{'K'}, std::byte{3},
                             std::byte{4}};
  explicit Input(bool public_claim = false) {
    if (public_claim) manifest["unauthenticated_prefixes"] = {"/dav/*"};
    refresh();
  }
  auto refresh() -> void {
    raw = manifest.dump();
    auto parsed = packages::HttpDeclaration::parse(raw);
    REQUIRE(parsed.value);
    declaration = std::move(*parsed.value);
  }
  auto request(packages::HttpInstallOperation operation =
                   packages::HttpInstallOperation::INSTALL,
               std::string_view predecessor = {},
               packages::Provenance provenance = packages::Provenance::USER)
      const -> packages::HttpInstallAuthorizationRequest {
    return {.zip_blob = zip,
            .raw_manifest = raw,
            .declaration = declaration,
            .package_name = "notes",
            .package_version = "1.2.3",
            .operation = operation,
            .predecessor_id = predecessor,
            .provenance = provenance};
  }
  auto confirmation(std::string_view actor = ACTOR) const -> std::string {
    return packages::http_public_confirmation_sha256(
        packages::http_install_zip_sha256(zip),
        packages::http_install_declarations_sha256(declaration), actor);
  }
};

struct PgFixture {
  plinth::Config config{plinth::ws_test::test_config()};
  packages::InstallerContext context;
  std::shared_ptr<PGconn> connection;
  std::filesystem::path root;
  PgFixture() {
    plinth::ws_test::settle_fixture_work();
    plinth::ws_test::reset_schema(config.db);
    plinth::groups::bootstrap_groups(config.db);
    std::array<char, 80> pattern{};
    std::ranges::copy(std::string_view{"/tmp/plinth-http-install-auth.XXXXXX"},
                      pattern.begin());
    const auto* directory = ::mkdtemp(pattern.data());
    REQUIRE(directory);
    root = directory;
    context.db = config.db;
    context.data_dir = root / "data";
    context.staging_dir = root / "staging";
    context.schedule_rbac_tests = false;
    std::filesystem::create_directories(context.data_dir);
    std::filesystem::create_directories(context.staging_dir);
    connection = std::shared_ptr<PGconn>{
        PQconnectdb(plinth::db::connection_info(config.db).c_str()), PQfinish};
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  }
  ~PgFixture() {
    plinth::ws_test::settle_fixture_work_or_exit();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  PgFixture(const PgFixture&) = delete;
  auto operator=(const PgFixture&) -> PgFixture& = delete;
  auto command(const std::string& sql) -> void {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto scalar(const std::string& sql) -> std::string {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    return PQgetvalue(result.get(), 0, 0);
  }
  auto rules(std::string_view actor) -> std::vector<std::string> {
    const std::string id{actor};
    const char* value = id.c_str();
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexecParams(connection.get(),
                     "SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
                     "JOIN plinth.group_rules gr ON gr.rule_id=r.id "
                     "JOIN plinth.groups g ON g.id=gr.group_id "
                     "LEFT JOIN plinth.group_members gm ON gm.group_id=g.id "
                     "WHERE r.orphaned_at IS NULL AND (gm.user_id=$1::uuid OR "
                     "g.name='everyone') "
                     "ORDER BY r.rule",
                     1, nullptr, &value, nullptr, nullptr, 0),
        PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    std::vector<std::string> output;
    output.reserve(static_cast<std::size_t>(PQntuples(result.get())));
    for (int row = 0; row < PQntuples(result.get()); ++row)
      output.emplace_back(PQgetvalue(result.get(), row, 0));
    return output;
  }
};

auto post_dry_run(PgFixture& fixture, const std::vector<std::byte>& zip,
                  const std::vector<std::string>& rules,
                  std::string confirmation = {}) -> drogon::HttpResponsePtr {
  auto request = actor_request(rules);
  request->setMethod(drogon::Post);
  request->setPath("/api/packages");
  request->setParameter("dry_run", "1");
  request->addHeader("Content-Type",
                     "multipart/form-data; boundary=plinth-http-auth");
  if (!confirmation.empty())
    request->addHeader("X-Plinth-Confirm-Public-HTTP", confirmation);
  std::string body{
      "--plinth-http-auth\r\nContent-Disposition: form-data; name=\"package\"; "
      "filename=\"fixture.zip\"\r\nContent-Type: application/zip\r\n\r\n"};
  body.append(reinterpret_cast<const char*>(zip.data()), zip.size());
  body += "\r\n--plinth-http-auth--\r\n";
  request->setBody(std::move(body));
  packages::PackageRoutesConfig config{.db = fixture.context.db,
                                       .data_dir = fixture.context.data_dir,
                                       .staging_dir =
                                           fixture.context.staging_dir};
  drogon::HttpResponsePtr response;
  packages::test_seam::dispatch_post(
      request, [&](const auto& result) { response = result; }, config);
  REQUIRE(response);
  return response;
}
} // namespace

TEST_CASE("HTTP install auth: missing authentic snapshots never mint proof",
          "[http-install-auth][unit]") {
  Input input;
  for (const auto& request :
       {drogon::HttpRequest::newHttpRequest(), actor_request({}),
        actor_request(private_rules(), "")}) {
    const auto result = packages::make_http_install_authorizer(
        request, input.zip)(input.request());
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == Code::PERMISSION_DENIED);
  }
  REQUIRE_FALSE(packages::make_http_install_authorizer(nullptr, input.zip)(
      input.request()));
}

TEST_CASE("HTTP install auth: required rules are conjunctive despite route ANY "
          "semantics",
          "[http-install-auth][unit]") {
  Input input;
  for (const auto& rules :
       {std::vector<std::string>{"packages.install"},
        std::vector<std::string>{"packages.install.with_http_prefixes"}}) {
    auto result = packages::make_http_install_authorizer(
        actor_request(rules), input.zip)(input.request());
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == Code::PERMISSION_DENIED);
  }
  REQUIRE(packages::make_http_install_authorizer(actor_request(private_rules()),
                                                 input.zip)(input.request()));
}

TEST_CASE("HTTP install auth: proof retains exact owned payload and operation",
          "[http-install-auth][unit]") {
  Input input;
  auto result = packages::make_http_install_authorizer(
      actor_request(private_rules()), input.zip)(input.request());
  REQUIRE(result);
  CHECK(result->actor_user_id() == ACTOR);
  CHECK(result->package_name() == "notes");
  CHECK(result->package_version() == "1.2.3");
  CHECK(result->operation() == packages::HttpInstallOperation::INSTALL);
  CHECK(result->predecessor_id().empty());
  CHECK(result->provenance() == packages::Provenance::USER);
  CHECK_FALSE(result->kernel_selected_bundle());
  CHECK(result->declaration() == input.declaration);
  CHECK(result->zip_sha256().size() == 64);
  CHECK(result->raw_manifest_sha256().size() == 64);
  CHECK(result->declarations_sha256().size() == 64);
  CHECK(result->matches(input.request()));
  CHECK_FALSE(result->matches(
      input.request(packages::HttpInstallOperation::UPGRADE, PREDECESSOR)));
  input.raw += ' ';
  CHECK_FALSE(result->matches(input.request()));
}

TEST_CASE("HTTP install auth: public rule and exact acknowledgement are both "
          "necessary",
          "[http-install-auth][unit]") {
  Input input{true};
  auto request = actor_request(private_rules());
  request->addHeader("X-Plinth-Confirm-Public-HTTP", input.confirmation());
  auto denied = packages::make_http_install_authorizer(request, input.zip)(
      input.request());
  REQUIRE_FALSE(denied);
  REQUIRE(denied.error().code == Code::PERMISSION_DENIED);
  request = actor_request(public_rules());
  auto missing = packages::make_http_install_authorizer(request, input.zip)(
      input.request());
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code == Code::CONFIRMATION_REQUIRED);
  request->addHeader("X-Plinth-Confirm-Public-HTTP", input.confirmation());
  REQUIRE(packages::make_http_install_authorizer(request,
                                                 input.zip)(input.request()));
}

TEST_CASE(
    "HTTP install auth: admin remains universal but cannot skip public intent",
    "[http-install-auth][unit]") {
  Input private_input;
  auto request = actor_request({"kernel.admin"});
  REQUIRE(packages::make_http_install_authorizer(request, private_input.zip)(
      private_input.request()));
  Input public_input{true};
  auto result = packages::make_http_install_authorizer(
      request, public_input.zip)(public_input.request());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == Code::CONFIRMATION_REQUIRED);
}

TEST_CASE("HTTP install auth: changed ZIP actor and declarations invalidate "
          "confirmation",
          "[http-install-auth][unit]") {
  Input input{true};
  auto request = actor_request(public_rules());
  request->addHeader("X-Plinth-Confirm-Public-HTTP", input.confirmation());
  auto authorizer = packages::make_http_install_authorizer(request, input.zip);
  input.zip.push_back(std::byte{0});
  auto replaced = authorizer(input.request());
  REQUIRE_FALSE(replaced);
  REQUIRE(replaced.error().code == Code::INVALID_SUBMISSION);
  auto changed_upload = packages::make_http_install_authorizer(
      request, input.zip)(input.request());
  REQUIRE_FALSE(changed_upload);
  REQUIRE(changed_upload.error().code == Code::CONFIRMATION_REQUIRED);
  input.zip.pop_back();
  auto other = actor_request(public_rules(), OTHER_ACTOR);
  other->addHeader("X-Plinth-Confirm-Public-HTTP", input.confirmation());
  REQUIRE_FALSE(packages::make_http_install_authorizer(other, input.zip)(
      input.request()));
  input.manifest["http_prefixes"] = {"/other/*"};
  input.manifest["unauthenticated_prefixes"] = {"/other/*"};
  input.refresh();
  REQUIRE_FALSE(packages::make_http_install_authorizer(request, input.zip)(
      input.request()));
}

TEST_CASE("HTTP install auth: snapshot lifetime does not retain mutable "
          "request authority",
          "[http-install-auth][unit]") {
  Input input;
  auto request = actor_request(private_rules());
  auto authorizer = packages::make_http_install_authorizer(request, input.zip);
  request->attributes()->insert(plinth::auth::ATTR_USER_ID,
                                std::string{OTHER_ACTOR});
  request->attributes()->insert(plinth::rbac::ATTR_EFFECTIVE_RULES,
                                std::vector<std::string>{});
  request.reset();
  auto result = authorizer(input.request());
  REQUIRE(result);
  REQUIRE(result->actor_user_id() == ACTOR);
}

TEST_CASE("HTTP install auth: supplied declarations identity and predecessor "
          "are checked",
          "[http-install-auth][unit]") {
  Input input;
  auto authorizer = packages::make_http_install_authorizer(
      actor_request(private_rules()), input.zip);
  auto request = input.request();
  request.package_name = "other";
  REQUIRE_FALSE(authorizer(request));
  auto wrong_predecessor = input.request();
  wrong_predecessor.predecessor_id = PREDECESSOR;
  REQUIRE_FALSE(authorizer(wrong_predecessor));
  REQUIRE_FALSE(authorizer(
      input.request(packages::HttpInstallOperation::UPGRADE, "not-a-uuid")));
  const auto upgraded = authorizer(
      input.request(packages::HttpInstallOperation::UPGRADE, PREDECESSOR));
  REQUIRE(upgraded);
  CHECK(upgraded->predecessor_id() == PREDECESSOR);
  input.declaration.http_prefixes = {"/invented/*"};
  REQUIRE_FALSE(authorizer(input.request()));
}

TEST_CASE(
    "HTTP install auth: USER and empty actors cannot select bundled authority",
    "[http-install-auth][unit]") {
  Input input;
  auto request = actor_request({"kernel.admin"});
  REQUIRE_FALSE(packages::make_http_install_authorizer(request, input.zip)(
      input.request(packages::HttpInstallOperation::INSTALL, {},
                    packages::Provenance::BUNDLED)));
  REQUIRE_FALSE(packages::make_http_install_authorizer(
      actor_request({"kernel.admin"}, ""), input.zip)(input.request()));
}

TEST_CASE(
    "HTTP install auth: empty declaration needs only ordinary installer rules",
    "[http-install-auth][unit]") {
  Input input;
  input.manifest.erase("http_prefixes");
  input.manifest.erase("handler_mode");
  input.refresh();
  auto result = packages::make_http_install_authorizer(
      actor_request({"packages.install"}), input.zip)(input.request());
  REQUIRE(result);
  REQUIRE(result->declaration().http_prefixes.empty());
}

TEST_CASE("HTTP install auth PG: rule definitions grant nothing and real API "
          "dry-run checks them",
          "[http-install-auth][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgFixture fixture;
  REQUIRE(fixture.scalar("SELECT count(*) FROM plinth.rbac_rules WHERE rule IN "
                         "('packages.install.with_http_prefixes','packages."
                         "install.with_unauthenticated_prefixes')") == "2");
  REQUIRE(
      fixture.scalar(
          "SELECT count(*) FROM plinth.group_rules gr JOIN plinth.rbac_rules r "
          "ON r.id=gr.rule_id WHERE r.rule IN "
          "('packages.install.with_http_prefixes',"
          "'packages.install.with_unauthenticated_prefixes')") == "0");
  const auto check_registration_events = [&] {
    for (const auto* rule :
         {"packages.install.with_http_prefixes",
          "packages.install.with_unauthenticated_prefixes"}) {
      REQUIRE(fixture.scalar("SELECT count(*) FROM plinth.audit_log WHERE "
                             "action='rbac.rule_registered' "
                             "AND detail->>'rule'='" +
                             std::string{rule} + "'") == "1");
    }
  };
  check_registration_events();
  // Restart invokes the same kernel bootstrap entry. Repeating that entry
  // must neither duplicate registration evidence nor introduce any grant.
  plinth::groups::bootstrap_groups(fixture.config.db);
  check_registration_events();
  REQUIRE(fixture.scalar("SELECT count(*) FROM plinth.rbac_rules WHERE rule IN "
                         "('packages.install.with_http_prefixes',"
                         "'packages.install.with_unauthenticated_prefixes')") ==
          "2");
  REQUIRE(
      fixture.scalar(
          "SELECT count(*) FROM plinth.group_rules gr JOIN plinth.rbac_rules r "
          "ON r.id=gr.rule_id WHERE r.rule IN "
          "('packages.install.with_http_prefixes',"
          "'packages.install.with_unauthenticated_prefixes')") == "0");
  fixture.command(
      "INSERT INTO plinth.users(id,username,password_hash) VALUES('" +
      std::string{ACTOR} + "','http-installer','fixture-password-not-used')");
  fixture.command("INSERT INTO plinth.groups(name) VALUES('http-installers')");
  fixture.command(
      "INSERT INTO plinth.group_members(group_id,user_id) SELECT id,'" +
      std::string{ACTOR} +
      "'::uuid FROM plinth.groups WHERE name='http-installers'");
  const auto grant = [&](std::string_view rule) {
    fixture.command("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT "
                    "g.id,r.id FROM "
                    "plinth.groups g,plinth.rbac_rules r WHERE "
                    "g.name='http-installers' AND r.rule='" +
                    std::string{rule} + "'");
  };
  grant("packages.install");
  packages::static_test::PackageCopy package;
  auto manifest = package.json("manifest.json");
  manifest["http_prefixes"] = {"/dav/*"};
  manifest["handler_mode"] = "quickjs";
  package.write("manifest.json", manifest.dump());
  auto zip = package.zip();
  const auto require_unavailable =
      [&](const drogon::HttpResponsePtr& response) {
        REQUIRE(response->statusCode() == drogon::k422UnprocessableEntity);
        const auto offline = packages::validate(package.root());
        REQUIRE(offline.error_count() == 0);
        const auto cli =
            packages::static_test::cli_report(offline, package.root());
        auto messages = cli.at("messages");
        for (auto& finding : messages) {
          finding.erase("phase");
          for (const auto* field : {"path", "remediation"})
            if (finding.at(field).is_null()) finding.erase(field);
        }
        messages.push_back(
            {{"severity", "error"},
             {"rule", "http-handler-mode-unavailable"},
             {"path", "manifest.json/handler_mode"},
             {"message",
              "HTTP handler mode is unavailable in this kernel build"}});
        const nlohmann::json report{{"files_scanned", cli.at("files_scanned")},
                                    {"total_bytes", cli.at("total_bytes")},
                                    {"disposition", 1},
                                    {"messages", std::move(messages)}};
        const nlohmann::json expected{{"state", "INSTALL_FAILED"},
                                      {"failed_at_stage", "VALIDATING"},
                                      {"kind", "validation-errors"},
                                      {"message", "validation failed"},
                                      {"report", report}};
        CHECK(nlohmann::json::parse(response->body()) == expected);
      };
  CHECK(post_dry_run(fixture, zip, fixture.rules(ACTOR))->statusCode() ==
        drogon::k403Forbidden);
  grant("packages.install.with_http_prefixes");
  require_unavailable(post_dry_run(fixture, zip, fixture.rules(ACTOR)));
  manifest["unauthenticated_prefixes"] = {"/dav/*"};
  const auto raw = manifest.dump();
  package.write("manifest.json", raw);
  zip = package.zip();
  CHECK(post_dry_run(fixture, zip, fixture.rules(ACTOR))->statusCode() ==
        drogon::k403Forbidden);
  grant("packages.install.with_unauthenticated_prefixes");
  CHECK(post_dry_run(fixture, zip, fixture.rules(ACTOR))->statusCode() ==
        drogon::k409Conflict);
  const auto parsed = packages::HttpDeclaration::parse(raw);
  REQUIRE(parsed.value);
  const auto confirmation = packages::http_public_confirmation_sha256(
      packages::http_install_zip_sha256(zip),
      packages::http_install_declarations_sha256(*parsed.value), ACTOR);
  require_unavailable(
      post_dry_run(fixture, zip, fixture.rules(ACTOR), confirmation));
  CHECK(fixture.scalar("SELECT count(*) FROM plinth.packages") == "0");
  CHECK(std::filesystem::is_empty(fixture.context.staging_dir));
}

TEST_CASE("HTTP install auth PG: direct caller identity cannot mutate HTTP but "
          "legacy stays ordinary",
          "[http-install-auth][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgFixture fixture;
  packages::static_test::PackageCopy package;
  const auto legacy = package.json("manifest.json");
  auto manifest = legacy;
  manifest["http_prefixes"] = {"/dav/*"};
  manifest["handler_mode"] = "quickjs";
  package.write("manifest.json", manifest.dump());
  const auto zip = package.zip();
  for (const auto actor : {ACTOR, std::string_view{}}) {
    fixture.context.caller_user_id = actor;
    const auto result = packages::install_package(
        zip, packages::Provenance::USER, fixture.context);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == "permission-denied");
    REQUIRE(result.error().package_id.empty());
  }
  const auto fake_bundle = packages::install_package(
      zip, packages::Provenance::BUNDLED, fixture.context);
  REQUIRE_FALSE(fake_bundle);
  REQUIRE(fake_bundle.error().kind == "permission-denied");
  CHECK(fixture.scalar("SELECT count(*) FROM plinth.packages") == "0");
  CHECK_FALSE(std::filesystem::exists(fixture.context.data_dir / "extensions" /
                                      "notes"));
  CHECK(std::filesystem::is_empty(fixture.context.staging_dir));
  package.write("manifest.json", legacy.dump());
  auto ordinary = packages::install_package(
      package.zip(), packages::Provenance::USER, fixture.context, true);
  REQUIRE(ordinary);
  CHECK(ordinary->state == packages::InstallStage::VALIDATING);
  CHECK(fixture.scalar("SELECT count(*) FROM plinth.packages") == "0");
}
