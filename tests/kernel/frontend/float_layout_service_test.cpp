#include "../js/async_bridge_fixture.hpp"
#include "../ws/ws_test_fixture.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/frontend/float_layout_service.hpp"
#include "kernel/frontend/platform_startup.hpp"
#include "kernel/frontend/typed_assets.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/installed_generation_registry.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/shell/firstboot.hpp"
#include "kernel/shell/platform_graph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <json/writer.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace frontend = plinth::frontend;
namespace packages = plinth::packages;
namespace generation = packages::generation;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr auto ORIGIN = "http://127.0.0.1:28099";
constexpr auto MIME = "application/x-plinth-float-fixture";

struct TestTree {
  fs::path root;
  TestTree() {
    std::array<char, 80> pattern{};
    std::ranges::copy(std::string_view{"/tmp/plinth-float-service.XXXXXX"},
                      pattern.begin());
    const char* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("service test tree creation");
    root = created;
  }
  ~TestTree() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  TestTree(const TestTree&) = delete;
  auto operator=(const TestTree&) -> TestTree& = delete;
};

// Same libzip owned-buffer construction as existing package test helpers;
// package bytes come from the actual ordinary browser extension fixture.
auto provider_zip(
    std::vector<std::pair<std::string, std::string>>& owned_routes)
    -> std::vector<std::byte> {
  const fs::path root{std::string{CMAKE_SOURCE_DIR} +
                      "/tests/extensions/float-provider"};
  std::map<std::string, std::string> files;
  for (const auto& item : fs::recursive_directory_iterator(root)) {
    if (!item.is_regular_file()) continue;
    std::ifstream input{item.path(), std::ios::binary};
    REQUIRE(input.is_open());
    files.emplace(fs::relative(item.path(), root).generic_string(),
                  std::string{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}});
    REQUIRE_FALSE(input.bad());
  }
  REQUIRE(files.contains("manifest.json"));
  const auto manifest = nlohmann::json::parse(files.at("manifest.json"));
  // Record identity from the SAME bytes going into the ZIP, before install
  // can publish a route and then throw or return failure.
  owned_routes.emplace_back(manifest.at("name").get<std::string>(),
                            manifest.at("version").get<std::string>());
  std::unique_ptr<zip_source_t, decltype(&zip_source_free)> buffer{
      zip_source_buffer_create(nullptr, 0, 0, nullptr), zip_source_free};
  REQUIRE(buffer);
  zip_source_keep(buffer.get());
  std::unique_ptr<zip_t, decltype(&zip_discard)> archive{
      zip_open_from_source(buffer.get(), ZIP_TRUNCATE, nullptr), zip_discard};
  if (!archive) {
    zip_source_free(buffer.get());
    FAIL("provider fixture archive creation failed");
  }
  for (const auto& [name, bytes] : files) {
    std::unique_ptr<zip_source_t, decltype(&zip_source_free)> entry{
        zip_source_buffer(archive.get(), bytes.data(), bytes.size(), 0),
        zip_source_free};
    REQUIRE(entry);
    REQUIRE(zip_file_add(archive.get(), name.c_str(), entry.get(), 0) >= 0);
    static_cast<void>(entry.release());
  }
  REQUIRE(zip_close(archive.get()) == 0);
  static_cast<void>(archive.release());
  REQUIRE(zip_source_open(buffer.get()) == 0);
  struct Close {
    zip_source_t& source;
    ~Close() { zip_source_close(&source); }
  } close{*buffer};
  REQUIRE(zip_source_seek(buffer.get(), 0, SEEK_END) == 0);
  const auto size = zip_source_tell(buffer.get());
  REQUIRE(size > 0);
  REQUIRE(zip_source_seek(buffer.get(), 0, SEEK_SET) == 0);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  REQUIRE(zip_source_read(buffer.get(), bytes.data(), bytes.size()) == size);
  return bytes;
}

struct RuntimeReset {
  std::vector<std::pair<std::string, std::string>> owned_routes;
  ~RuntimeReset() {
    plinth::ws_test::settle_fixture_work_or_exit();
    // This member runs even if Fixture construction fails after installation.
    // Do not seal the process-wide asset server used by later test cases.
    for (const auto& [name, version] : owned_routes)
      packages::asset_server::unregister_routes(name, version);
    static_cast<void>(plinth::extensions::shutdown_registry());
    plinth::shell::test_seam::set_active_frontend(std::nullopt);
    plinth::capabilities::drain::end_drain("shell");
    plinth::capabilities::drain::end_drain("floatprovider");
  }
};

struct Fixture {
  TestTree tree;
  plinth::Config config{plinth::ws_test::test_config()};
  // Runs even if construction/assertion fails; AFTER service members but
  // BEFORE the config backing the process registry and owned tree disappear.
  RuntimeReset reset;
  packages::InstallerContext context;
  std::shared_ptr<generation::InstalledGenerationRegistry> registry;
  std::shared_ptr<frontend::TypedAssetStreams> streams;
  std::unique_ptr<frontend::FloatLayoutService> service;
  std::unique_ptr<plinth::ws_test::TestPg> pg;
  std::optional<plinth::shell::ActiveFrontend> platform;
  packages::PackageRecord provider;
  plinth::auth::AuthContext auth;
  bool expected_delivery_failure = false;

  Fixture() {
    plinth::ws_test::settle_fixture_work();
    static_cast<void>(plinth::extensions::shutdown_registry());
    plinth::shell::test_seam::set_active_frontend(std::nullopt);
    plinth::capabilities::drain::end_drain("shell");
    plinth::capabilities::drain::end_drain("floatprovider");
    plinth::ws_test::reset_schema(config.db);
    context.db = config.db;
    context.data_dir = tree.root / "data";
    context.staging_dir = tree.root / "staging";
    context.schedule_rbac_tests = false;
    fs::create_directories(context.data_dir);
    fs::create_directories(context.staging_dir);
    config.packages_data_dir = context.data_dir.string();
    config.packages_staging_dir = context.staging_dir.string();
    config.shell.bundle_path =
        std::string{CMAKE_BINARY_DIR} + "/share/plinth/bundled";
    REQUIRE(plinth::capabilities::init_resolver(config.db));
    plinth::extensions::init_registry(config);
    registry = std::make_shared<generation::InstalledGenerationRegistry>(
        context.data_dir);
    context.generation_registry = registry;
    const auto bundle = plinth::shell::bundled_shell_status(config);
    REQUIRE(bundle);
    reset.owned_routes.emplace_back("shell", bundle->available_version);
    REQUIRE(plinth::shell::ensure_bundled_shell_installed(config, context));
    const auto prepared = frontend::prepare_platform_startup(
        config, registry, std::chrono::steady_clock::now() + 10s);
    REQUIRE(prepared);
    REQUIRE(*prepared);
    platform = **prepared;
    REQUIRE(platform->platform);
    const auto bytes = provider_zip(reset.owned_routes);
    const auto installed =
        packages::install_package(bytes, packages::Provenance::USER, context);
    REQUIRE(installed);
    provider = *installed;
    pg = std::make_unique<plinth::ws_test::TestPg>(config.db);
    REQUIRE(PQstatus(pg->conn) == CONNECTION_OK);
    const auto user = plinth::ws_test::insert_user(*pg, "float_service_owner",
                                                   "fake-test-password");
    const std::string hash(64, 'a');
    const auto session = pg->exec_params(
        "INSERT INTO plinth.sessions(user_id,token_hash) VALUES($1::uuid,$2) "
        "RETURNING id::text",
        {user, hash});
    REQUIRE(PQresultStatus(session.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(session.get()) == 1);
    auth = {.user_id = user,
            .username = "float_service_owner",
            .auth_type = "session",
            .session_id = PQgetvalue(session.get(), 0, 0),
            .pat_id = {},
            .token_hash = hash,
            .credential_source = plinth::auth::CredentialSource::COOKIE};
    command("INSERT INTO plinth.groups(name,description) VALUES"
            "('float_service_viewers','controlled real service grants')");
    parameters(
        "INSERT INTO plinth.group_members(group_id,user_id) SELECT id,$1::uuid "
        "FROM plinth.groups WHERE name='float_service_viewers'",
        {user});
    command("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
            "FROM plinth.groups g CROSS JOIN plinth.rbac_rules r "
            "WHERE g.name='float_service_viewers' AND "
            "r.rule IN ('floatprovider.panel','floatprovider.preview')");
    streams = std::make_shared<frontend::TypedAssetStreams>(
        registry->source_budget());
    service = std::make_unique<frontend::FloatLayoutService>(config, registry,
                                                             nullptr, streams);
  }
  ~Fixture() {
    // Also handles assertion unwinding: accepted sources are scoped after this
    // fixture and therefore disappear before these dependency owners.
    if (service) {
      const bool drained =
          service->drain(std::chrono::steady_clock::now() + 5s);
      if (!drained && !expected_delivery_failure) std::terminate();
      service
          .reset(); // Destructor still requires physical join and zero slots.
    }
    registry->clear();
  }
  Fixture(const Fixture&) = delete;
  auto operator=(const Fixture&) -> Fixture& = delete;
  auto command(const std::string& sql) -> void {
    const auto result = pg->exec(sql);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto parameters(const std::string& sql,
                  const std::vector<std::string>& values) -> void {
    const auto result = pg->exec_params(sql, values);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto scalar(const std::string& sql) -> std::string {
    const auto result = pg->exec(sql);
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    REQUIRE_FALSE(PQgetisnull(result.get(), 0, 0));
    return PQgetvalue(result.get(), 0, 0);
  }
  template <typename Submit>
  auto json(Submit submit) -> frontend::FloatLayoutService::Result {
    auto completion =
        std::make_shared<std::promise<frontend::FloatLayoutService::Result>>();
    auto result = completion->get_future();
    submit(
        [completion](auto value) { completion->set_value(std::move(value)); });
    REQUIRE(result.wait_for(12s) == std::future_status::ready);
    return result.get();
  }
  auto catalog(std::optional<std::string> cursor = std::nullopt,
               std::optional<std::string> type = std::nullopt) {
    return json([&](auto completion) {
      service->catalog(
          {.filters = {.content_type = std::move(type), .limit = 1},
           .cursor = std::move(cursor),
           .trusted_origin = ORIGIN},
          auth, "127.0.0.1", std::move(completion));
    });
  }
  auto admit(std::vector<frontend::SurfaceLookup> items) {
    return json([&](auto completion) {
      service->admit(std::move(items), ORIGIN, auth, "127.0.0.1",
                     std::move(completion));
    });
  }
  auto read() {
    return json([&](auto completion) {
      service->read_layout(ORIGIN, auth, "127.0.0.1", std::move(completion));
    });
  }
  auto selector(std::string_view member = "client/panels/preview.js")
      -> frontend::protocol::AssetSelector {
    const auto page = catalog(std::nullopt, MIME);
    REQUIRE(page);
    REQUIRE((*page)["targets"].size() == 1);
    const auto parsed = frontend::protocol::parse_asset_selector(
        (*page)["targets"][0]["module_url"].asString());
    REQUIRE(parsed);
    auto result = *parsed;
    result.member_path = member;
    return result;
  }
  auto asset(
      frontend::protocol::AssetSelector selector,
      plinth::auth::AuthContext caller, drogon::HttpMethod method = drogon::Get,
      std::shared_ptr<plinth::lifecycle::AsyncTaskLease> accepted = nullptr)
      -> frontend::FloatLayoutService::AssetResult {
    auto completion = std::make_shared<
        std::promise<frontend::FloatLayoutService::AssetResult>>();
    auto result = completion->get_future();
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(method);
    service->serve_asset(
        std::move(selector), std::move(request), std::move(caller), "127.0.0.1",
        [completion](auto value) { completion->set_value(std::move(value)); },
        std::move(accepted));
    REQUIRE(result.wait_for(12s) == std::future_status::ready);
    return result.get();
  }
};

auto lookup(std::string panel = "preview", std::string mime = MIME)
    -> frontend::SurfaceLookup {
  return {.application_id = "floatprovider",
          .panel_id = std::move(panel),
          .kind = frontend::SurfaceKind::FLOAT,
          .expected_generation = std::nullopt,
          .content_type = std::move(mime),
          .capability = std::nullopt};
}
auto entry() -> nlohmann::json {
  return {{"application_id", "floatprovider"},
          {"panel_id", "preview"},
          {"capability", "floatprovider:1:preview"},
          {"context", {{"record_id", "fake-record"}}},
          {"presentation", "shown"},
          {"maximized", false},
          {"geometry", {{"x", 0}, {"y", 0}, {"width", 320}, {"height", 240}}}};
}
} // namespace

TEST_CASE("float layout service: real installed catalog admission and "
          "credential refresh",
          "[frontend][float_layout_service][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  Fixture fixture;
  SECTION(
      "prefix cursor and fresh indexed available unavailable stale outcomes") {
    const auto first = fixture.catalog();
    REQUIRE(first);
    REQUIRE((*first)["targets"].size() == 1);
    REQUIRE((*first)["targets"][0]["panel_id"].asString() == "live");
    REQUIRE((*first)["next_cursor"].isString());
    const auto second = fixture.catalog((*first)["next_cursor"].asString());
    REQUIRE(second);
    REQUIRE((*second)["targets"].size() == 1);
    REQUIRE((*second)["targets"][0]["panel_id"].asString() == "preview");
    REQUIRE((*second)["next_cursor"].isNull());
    const auto admitted =
        fixture.admit({lookup(), lookup("absent"),
                       lookup("live", "application/x-plinth-float-live")});
    REQUIRE(admitted);
    REQUIRE((*admitted)["results"].size() == 3);
    for (Json::ArrayIndex index = 0; index < 3; ++index)
      REQUIRE((*admitted)["results"][index]["index"].asUInt() == index);
    REQUIRE((*admitted)["results"][0]["status"].asString() == "available");
    REQUIRE(
        (*admitted)["results"][0]["target"]["persistence_contract"].isNull());
    REQUIRE((*admitted)["results"][1].size() == 2);
    REQUIRE((*admitted)["results"][1]["status"].asString() == "unavailable");
    REQUIRE((*admitted)["results"][2]["status"].asString() == "available");
    auto pinned = lookup();
    pinned.expected_generation = "00000000-0000-0000-0000-000000000046";
    pinned.content_type.reset();
    pinned.capability = "floatprovider:1:preview";
    const auto stale = fixture.admit({pinned});
    REQUIRE(stale);
    REQUIRE((*stale)["results"][0]["status"].asString() == "stale");
    REQUIRE((*stale)["results"][0].size() == 2);
    const auto mismatch = fixture.admit({lookup("preview", "text/plain")});
    REQUIRE(mismatch);
    REQUIRE((*mismatch)["results"][0]["status"].asString() == "unavailable");
  }
  SECTION("both actual rules and fresh membership remain mandatory") {
    REQUIRE(fixture.catalog(std::nullopt, MIME));
    for (const auto rule : {"floatprovider.preview", "floatprovider.panel"}) {
      fixture.parameters("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT "
                         "id FROM plinth.rbac_rules WHERE rule=$1)",
                         {rule});
      const auto denied = fixture.catalog(std::nullopt, MIME);
      REQUIRE(denied);
      REQUIRE((*denied)["targets"].empty());
      const auto admission = fixture.admit({lookup()});
      REQUIRE(admission);
      REQUIRE((*admission)["results"][0]["status"].asString() == "unavailable");
      REQUIRE((*admission)["results"][0].size() == 2);
      fixture.parameters(
          "INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
          "FROM plinth.groups g CROSS JOIN plinth.rbac_rules r WHERE "
          "g.name='float_service_viewers' AND r.rule=$1",
          {rule});
    }
    fixture.parameters(
        "DELETE FROM plinth.group_members WHERE user_id=$1::uuid",
        {fixture.auth.user_id});
    const auto removed = fixture.catalog(std::nullopt, MIME);
    REQUIRE(removed);
    REQUIRE((*removed)["targets"].empty());
  }
  SECTION("credential hash and revocation are refreshed after prior success") {
    REQUIRE(fixture.catalog(std::nullopt, MIME));
    const auto original = fixture.auth.token_hash;
    fixture.auth.token_hash = std::string(64, 'b');
    const auto forged = fixture.catalog(std::nullopt, MIME);
    REQUIRE_FALSE(forged);
    REQUIRE(forged.error().code == "not_authenticated");
    fixture.auth.token_hash = original;
    fixture.parameters(
        "UPDATE plinth.sessions SET revoked_at=NOW() WHERE id=$1::uuid",
        {fixture.auth.session_id});
    const auto revoked = fixture.catalog(std::nullopt, MIME);
    REQUIRE_FALSE(revoked);
    REQUIRE(revoked.error().code == "not_authenticated");
    const auto denied = fixture.admit({lookup()});
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error().code == "not_authenticated");
  }
  SECTION("resolver-only administrator candidate uses native storage without "
          "impersonation") {
    const auto result = fixture.json([&](auto completion) {
      fixture.service->catalog({.filters = {.content_type = MIME,
                                            .limit = 1,
                                            .include_admin_default = true},
                                .cursor = std::nullopt,
                                .trusted_origin = ORIGIN},
                               fixture.auth, "127.0.0.1",
                               std::move(completion));
    });
    REQUIRE(result);
    REQUIRE((*result)["targets"].size() == 1);
    REQUIRE((*result).isMember("admin_default_candidate"));
    REQUIRE((*result)["admin_default_candidate"].isNull());
    REQUIRE(fixture.scalar(
                "SELECT count(*)::text FROM plinth.group_members m JOIN "
                "plinth.groups g ON g.id=m.group_id WHERE g.name='admin'") ==
            "0");
  }
}

TEST_CASE(
    "float layout service: native storage restore envelope and policy refusal",
    "[frontend][float_layout_service][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  Fixture fixture;
  const auto absent = fixture.read();
  REQUIRE(absent);
  REQUIRE((*absent).size() == 4);
  REQUIRE_FALSE((*absent)["present"].asBool());
  REQUIRE_FALSE((*absent).isMember("layout"));
  REQUIRE((*absent)["policy_revision"].isNull());
  REQUIRE((*absent)["restore_targets"].empty());
  SECTION("present JSON null and malformed roots remain present without "
          "fabricated targets") {
    for (const auto& value :
         {std::string{"null"},
          std::string{"{\"version\":1,\"floats\":[],\"extra\":true}"}}) {
      fixture.parameters(
          "INSERT INTO ext_shell.user_preferences(user_id,key,value) "
          "VALUES($1::uuid,'shell.floats',$2::jsonb) ON CONFLICT(user_id,key) "
          "DO UPDATE SET value=EXCLUDED.value",
          {fixture.auth.user_id, value});
      const auto read = fixture.read();
      REQUIRE(read);
      REQUIRE((*read)["present"].asBool());
      REQUIRE((*read).isMember("layout"));
      REQUIRE((*read)["restore_targets"].empty());
      Json::StreamWriterBuilder writer;
      writer["indentation"] = "";
      const auto expected = nlohmann::json::parse(value);
      const auto observed =
          nlohmann::json::parse(Json::writeString(writer, (*read)["layout"]));
      // QuickJS/native JSON may represent bounded numeric version 1 as 1.0.
      // Compare decoded JSON semantics, not JsonCpp's integer storage tag;
      // retain exact keys, arrays, boolean/null values and malformed extra.
      REQUIRE(observed == expected);
      if (expected.is_null()) {
        REQUIRE((*read)["layout"].isNull());
      } else {
        REQUIRE((*read)["layout"].isObject());
        REQUIRE((*read)["layout"].size() == 3);
        REQUIRE((*read)["layout"]["version"].isNumeric());
        REQUIRE((*read)["layout"]["version"].asDouble() == 1.0);
        REQUIRE((*read)["layout"]["floats"].isArray());
        REQUIRE((*read)["layout"]["floats"].empty());
        REQUIRE((*read)["layout"]["extra"].isBool());
        REQUIRE((*read)["layout"]["extra"].asBool());
      }
    }
  }
  SECTION(
      "bounded raw Unicode storage is not expanded into an escaped refusal") {
    std::string text;
    for (unsigned index = 0; index < 6000; ++index)
      text += "\u4e2d";
    const nlohmann::json original{
        {"version", 1}, {"floats", nlohmann::json::array()}, {"extra", text}};
    REQUIRE(original.dump().size() < 32768);
    REQUIRE(original.dump(-1, ' ', true).size() > 32768);
    fixture.parameters(
        "INSERT INTO ext_shell.user_preferences(user_id,key,value) "
        "VALUES($1::uuid,'shell.floats',$2::jsonb)",
        {fixture.auth.user_id, original.dump()});
    const auto read = fixture.read();
    REQUIRE(read);
    REQUIRE((*read)["present"].asBool());
    REQUIRE((*read)["layout"].size() == 3);
    REQUIRE((*read)["layout"]["extra"].asString() == text);
    REQUIRE((*read)["restore_targets"].empty());
  }
  SECTION("native stored descriptors keep original indexes while null policy "
          "refuses every restore") {
    const nlohmann::json layout{
        {"version", 1},
        {"floats", nlohmann::json::array({false, entry(), entry()})}};
    fixture.parameters(
        "INSERT INTO ext_shell.user_preferences(user_id,key,value) "
        "VALUES($1::uuid,'shell.floats',$2::jsonb)",
        {fixture.auth.user_id, layout.dump()});
    const auto read = fixture.read();
    REQUIRE(read);
    REQUIRE((*read)["present"].asBool());
    REQUIRE((*read)["layout"]["floats"].size() == 3);
    REQUIRE((*read)["layout"]["floats"][0].isBool());
    REQUIRE((*read)["restore_targets"].size() == 3);
    for (Json::ArrayIndex index = 0; index < 3; ++index) {
      REQUIRE((*read)["restore_targets"][index].size() == 2);
      REQUIRE((*read)["restore_targets"][index]["index"].asUInt() == index);
      REQUIRE((*read)["restore_targets"][index]["status"].asString() ==
              "unavailable");
    }
  }
  SECTION("even empty guarded reset needs actual protected policy and cannot "
          "issue DML") {
    fixture.parameters(
        "INSERT INTO ext_shell.user_preferences(user_id,key,value) "
        "VALUES($1::uuid,'shell.floats','null'::jsonb)",
        {fixture.auth.user_id});
    // Sequence increments survive transaction rollback, unlike a table count.
    // The isolated witness permission is not an application/RBAC grant.
    fixture.command("CREATE SEQUENCE ext_shell.test_write_witness");
    fixture.command("GRANT USAGE ON SEQUENCE ext_shell.test_write_witness TO "
                    "ext_shell_role");
    fixture.command(
        "CREATE FUNCTION ext_shell.test_write_seen() RETURNS trigger LANGUAGE "
        "plpgsql AS $$ BEGIN PERFORM nextval('ext_shell.test_write_witness'); "
        "RETURN NULL; END $$");
    fixture.command("CREATE TRIGGER test_write_seen AFTER INSERT OR UPDATE OR "
                    "DELETE ON ext_shell.user_preferences FOR EACH ROW EXECUTE "
                    "FUNCTION ext_shell.test_write_seen()");
    const auto parsed = frontend::layout_contract::parse_float_commit(
        R"({"schema_version":1,"layout":{"version":1,"floats":[]},"bindings":[]})");
    REQUIRE(parsed);
    const auto result = fixture.json([&](auto completion) {
      fixture.service->commit_layout(*parsed, fixture.auth, "127.0.0.1",
                                     std::move(completion));
    });
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == "storage_unavailable");
    REQUIRE(fixture.scalar(
                "SELECT is_called::text FROM ext_shell.test_write_witness") ==
            "false");
    REQUIRE(fixture.scalar("SELECT value::text FROM ext_shell.user_preferences "
                           "WHERE key='shell.floats'") == "null");
  }
}

TEST_CASE("float layout service: admitted asset physical ownership and public "
          "bootstrap",
          "[frontend][float_layout_service][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  Fixture fixture;
  SECTION("public exact captured platform loads before login but provider "
          "requires fresh auth") {
    const auto graph = fixture.platform->platform;
    const auto selector =
        frontend::protocol::parse_asset_selector(graph->base() + "sdk.js");
    REQUIRE(selector);
    const auto public_response = fixture.asset(*selector, {}, drogon::Head);
    REQUIRE(public_response);
    REQUIRE((*public_response)->statusCode() == drogon::k200OK);
    REQUIRE((*public_response)->getHeader("Content-Type") ==
            "text/javascript; charset=utf-8");
    REQUIRE_FALSE((*public_response)->streamCallback());
    auto unapproved = *selector;
    unapproved.member_path = "server/platform-storage-v1.js";
    const auto private_file = fixture.asset(unapproved, {}, drogon::Head);
    REQUIRE_FALSE(private_file);
    REQUIRE(private_file.error().code == "asset_not_found");
    auto stale_graph = *selector;
    stale_graph.artifact_digest[0] =
        stale_graph.artifact_digest[0] == 'a' ? 'b' : 'a';
    REQUIRE_FALSE(fixture.asset(stale_graph, {}, drogon::Head));
    const auto provider = fixture.selector();
    const auto denied = fixture.asset(provider, {}, drogon::Head);
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error().code == "not_authenticated");
    const auto allowed = fixture.asset(provider, fixture.auth, drogon::Head);
    REQUIRE(allowed);
    fixture.parameters(
        "UPDATE plinth.sessions SET revoked_at=NOW() WHERE id=$1::uuid",
        {fixture.auth.session_id});
    const auto revoked = fixture.asset(provider, fixture.auth, drogon::Head);
    REQUIRE_FALSE(revoked);
    REQUIRE(revoked.error().code == "not_authenticated");
    plinth::extensions::destroy_pool("shell");
    const auto no_binding = fixture.asset(*selector, {}, drogon::Head);
    REQUIRE_FALSE(no_binding);
  }
  SECTION("relative dependencies share exact captured generation and regular "
          "client boundary") {
    auto selector = fixture.selector();
    for (const auto member :
         {"client/panels/preview.js", "client/panels/label.js",
          "client/panels/detail.js"}) {
      selector.member_path = member;
      const auto response = fixture.asset(selector, fixture.auth, drogon::Head);
      REQUIRE(response);
      REQUIRE((*response)->statusCode() == drogon::k200OK);
      REQUIRE((*response)->getHeader("ETag") ==
              ('"' + selector.artifact_digest + '"'));
    }
    for (const auto member :
         {"server/main.js", "client/panels", "client/missing.js"}) {
      selector.member_path = member;
      const auto denied = fixture.asset(selector, fixture.auth, drogon::Head);
      REQUIRE_FALSE(denied);
      REQUIRE(denied.error().code == "asset_not_found");
    }
    selector.member_path = "client/panels/preview.js";
    selector.artifact_digest[0] =
        selector.artifact_digest[0] == 'a' ? 'b' : 'a';
    const auto stale = fixture.asset(selector, fixture.auth, drogon::Head);
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == "asset_not_found");
  }
  SECTION("slow transport copy retains actual service slot guard source and "
          "accepted lease") {
    plinth::lifecycle::AsyncTaskRegistry accepted;
    const auto selector = fixture.selector();
    auto response = fixture.asset(selector, fixture.auth, drogon::Get,
                                  accepted.try_acquire());
    REQUIRE(response);
    auto transport = (*response)->streamCallback();
    REQUIRE(transport);
    (*response)->clear();
    response->reset();
    REQUIRE(fixture.streams->active_count() == 1);
    REQUIRE(fixture.service->active_count() == 1);
    REQUIRE(accepted.active_for_test() == 1);
    const auto fence =
        plinth::capabilities::drain::begin_drain("floatprovider");
    REQUIRE(fence->in_flight.load() == 1);
    std::array<char, 65536> bytes{};
    REQUIRE(transport(bytes.data(), bytes.size()) > 0);
    REQUIRE(transport(bytes.data(), bytes.size()) == 0);
    REQUIRE(transport(nullptr, 0) == 0);
    REQUIRE(fixture.streams->active_count() == 1);
    REQUIRE(accepted.active_for_test() == 1);
    REQUIRE_FALSE(fixture.service->drain(std::chrono::steady_clock::now()));
    REQUIRE_FALSE(accepted.drain(0ms));
    REQUIRE(fence->in_flight.load() == 1);
    transport = {};
    REQUIRE(fixture.streams->active_count() == 0);
    REQUIRE(fixture.service->active_count() == 0);
    REQUIRE(fence->in_flight.load() == 0);
    REQUIRE(accepted.drain(0ms));
    REQUIRE(fixture.service->drain(std::chrono::steady_clock::now() + 5s));
  }
  SECTION(
      "throwing accepted completion is delivered once and drain stays failed") {
    fixture.expected_delivery_failure = true;
    auto seen = std::make_shared<std::atomic<unsigned>>(0);
    auto completion = std::make_shared<std::promise<bool>>();
    auto delivered = completion->get_future();
    fixture.service->catalog(
        {.filters = {.content_type = MIME, .limit = 1},
         .cursor = std::nullopt,
         .trusted_origin = ORIGIN},
        fixture.auth, "127.0.0.1", [seen, completion](auto result) {
          seen->fetch_add(1);
          completion->set_value(result.has_value());
          throw std::runtime_error("controlled accepted response failure");
        });
    REQUIRE(delivered.wait_for(12s) == std::future_status::ready);
    REQUIRE(delivered.get());
    REQUIRE_FALSE(
        fixture.service->drain(std::chrono::steady_clock::now() + 5s));
    REQUIRE(seen->load() == 1);
    REQUIRE(fixture.service->active_count() == 0);
    REQUIRE(fixture.streams->active_count() == 0);
    REQUIRE_FALSE(
        fixture.service->drain(std::chrono::steady_clock::now() + 5s));
  }
}
