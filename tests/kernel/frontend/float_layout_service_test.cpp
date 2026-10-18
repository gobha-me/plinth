#include "../js/async_bridge_fixture.hpp"
#include "../ws/ws_test_fixture.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"
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
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Existing actual route adapter only; it cannot manufacture an AdminReply.
namespace plinth::frontend::test_seam {
auto float_admin_completion(std::function<void(const drogon::HttpResponsePtr&)>,
                            bool) -> FloatLayoutService::AdminCompletion;
} // namespace plinth::frontend::test_seam

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
    std::vector<std::pair<std::string, std::string>>& owned_routes,
    bool add_nonfloat = false) -> std::vector<std::byte> {
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
  if (add_nonfloat) {
    auto panels = nlohmann::json::parse(files.at("panels.json"));
    // This test owns an exact two-float/two-tray/one-receiver catalog. The
    // shared native fixture can grow independent nonfloat declarations without
    // silently changing this test's strict ordering/lookahead assertions.
    auto original_floats = nlohmann::json::array();
    for (const auto& panel : panels.at("panels")) {
      if (panel.at("id") == "preview" || panel.at("id") == "live") {
        REQUIRE(panel.at("type") == "float");
        original_floats.push_back(panel);
      }
    }
    REQUIRE(original_floats.size() == 2);
    panels["panels"] = std::move(original_floats);
    const nlohmann::json schema{{"version", 1},
                                {"fields", nlohmann::json::object()},
                                {"persist_keys", nlohmann::json::array()}};
    panels["panels"].push_back({{"id", "receiver"},
                                {"type", "primary"},
                                {"title", "Typed Receiver"},
                                {"icon", "edit-3"},
                                {"rbac_rule", "floatprovider.panel"},
                                {"client_path", "receiver.js"},
                                {"accepts_navigation_intent", true},
                                {"context_schema", schema}});
    panels["panels"].push_back({{"id", "legacy"},
                                {"type", "primary"},
                                {"title", "Nonopted Primary"},
                                {"icon", "edit-3"},
                                {"rbac_rule", "floatprovider.panel"},
                                {"client_path", "receiver.js"}});
    // These are ordinary foreign declarations. Their flag and even shell-like
    // IDs never grant essential status, capability or persistent projection.
    for (const auto id : {"avatar", "bell"})
      panels["panels"].push_back({{"id", id},
                                  {"type", "tray"},
                                  {"title", "Foreign Tray"},
                                  {"icon", "edit-3"},
                                  {"rbac_rule", "floatprovider.panel"},
                                  {"client_path", "tray.js"},
                                  {"tray_states", {{"ready", "edit-3"}}},
                                  {"tray_initial_state", "ready"},
                                  {"preferred_width", 320},
                                  {"chrome_essential", true}});
    files["panels.json"] = panels.dump();
    files["client/panels/receiver.js"] =
        "export default function receiver() { return null; }\n";
    files["client/panels/tray.js"] =
        "export default function tray() { return null; }\n";
  }
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

  explicit Fixture(bool add_nonfloat = false) {
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
    const auto bytes = provider_zip(reset.owned_routes, add_nonfloat);
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
  auto grant_admin() -> void {
    command("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
            "FROM plinth.groups g CROSS JOIN plinth.rbac_rules r "
            "WHERE g.name='float_service_viewers' AND r.rule='kernel.admin'");
  }
  template <typename Submit>
  auto management(Submit submit) -> frontend::FloatLayoutService::AdminResult {
    auto completion = std::make_shared<
        std::promise<frontend::FloatLayoutService::AdminResult>>();
    auto result = completion->get_future();
    submit(
        [completion](auto value) { completion->set_value(std::move(value)); });
    REQUIRE(result.wait_for(12s) == std::future_status::ready);
    return result.get();
  }
  auto defaults_read(frontend::admin_defaults::ReadRequest input =
                         frontend::admin_defaults::ReadPage{},
                     int depth = 0) {
    return management([&](auto completion) {
      service->read_admin_defaults(std::move(input), auth, "127.0.0.1", depth,
                                   std::move(completion));
    });
  }
  auto defaults_put(std::string type = "text/plain", int depth = 0) {
    return management([&](auto completion) {
      service->put_admin_default(
          {std::move(type), {"uninstalled-app", "old_panel"}}, auth,
          "127.0.0.1", depth, std::move(completion));
    });
  }
  auto defaults_delete(std::string type = "text/plain") {
    return management([&](auto completion) {
      service->delete_admin_default({std::move(type)}, auth, "127.0.0.1", 0,
                                    std::move(completion));
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

struct AdminPause {
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool expired = false;
    auto release() -> void {
      std::lock_guard lock{mutex};
      released = true;
      changed.notify_all();
    }
  };
  frontend::FloatLayoutService& service;
  std::shared_ptr<Gate> gate = std::make_shared<Gate>();
  explicit AdminPause(frontend::FloatLayoutService& source) : service(source) {
    frontend::test_seam::set_admin_native_settled_hook(service, [state = gate] {
      std::unique_lock lock{state->mutex};
      state->entered = true;
      state->changed.notify_all();
      if (!state->changed.wait_for(lock, 5s, [&] { return state->released; }))
        state->expired = true;
    });
  }
  ~AdminPause() {
    gate->release(); // Release before fixture/service drain, including asserts.
    frontend::test_seam::set_admin_native_settled_hook(service, {});
  }
  AdminPause(const AdminPause&) = delete;
  auto operator=(const AdminPause&) -> AdminPause& = delete;
  auto wait_entered() -> bool {
    std::unique_lock lock{gate->mutex};
    return gate->changed.wait_for(lock, 5s, [&] { return gate->entered; });
  }
  auto expired() -> bool {
    std::lock_guard lock{gate->mutex};
    return gate->expired;
  }
};

// Assertion unwinding releases the native pause BEFORE joining the ordinary
// lifecycle thread, which otherwise needs the ACK guard to leave its drain.
struct ReleaseAdminBeforeJoin {
  std::shared_ptr<AdminPause::Gate> gate;
  std::jthread thread;
  ~ReleaseAdminBeforeJoin() {
    gate->release();
    if (thread.joinable()) thread.join();
  }
};

// A real success retained past worker completion must be released BEFORE
// joining the lifecycle which is waiting for its counted ACK guard, including
// assertion unwind. No synthetic frame/identity publication is involved.
struct RetainedAdminBeforeJoin {
  std::shared_ptr<AdminPause::Gate> gate;
  std::optional<frontend::FloatLayoutService::AdminResult> result;
  std::jthread thread;
  ~RetainedAdminBeforeJoin() {
    gate->release();
    result.reset();
    if (thread.joinable()) thread.join();
  }
};
} // namespace

TEST_CASE("float layout service: real installed catalog admission and "
          "credential refresh",
          "[frontend][float_layout_service][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  Fixture fixture{true};
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
    const auto before =
        frontend::test_seam::fixed_storage_dispatch_attempts(*fixture.service);
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
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before + 1);
    REQUIRE(fixture.scalar(
                "SELECT count(*)::text FROM plinth.group_members m JOIN "
                "plinth.groups g ON g.id=m.group_id WHERE g.name='admin'") ==
            "0");
  }
  SECTION("nonfloat native preflight performs zero fixed storage dispatches") {
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == 0);
    for (const auto kind :
         {frontend::SurfaceKind::PRIMARY, frontend::SurfaceKind::TRAY}) {
      for (const bool admin : {false, true}) {
        const auto refused = fixture.json([&](auto completion) {
          fixture.service->catalog({.filters = {.content_type = MIME,
                                                .limit = 1,
                                                .include_admin_default = admin,
                                                .kind = kind},
                                    .cursor = std::nullopt,
                                    .trusted_origin = ORIGIN},
                                   fixture.auth, "127.0.0.1",
                                   std::move(completion));
        });
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error().code == "invalid_argument");
      }
      const auto admin_only = fixture.json([&](auto completion) {
        fixture.service->catalog({.filters = {.limit = 1,
                                              .include_admin_default = true,
                                              .kind = kind},
                                  .cursor = std::nullopt,
                                  .trusted_origin = ORIGIN},
                                 fixture.auth, "127.0.0.1",
                                 std::move(completion));
      });
      REQUIRE_FALSE(admin_only);
      REQUIRE(admin_only.error().code == "invalid_argument");
      const auto valid = fixture.json([&](auto completion) {
        fixture.service->catalog({.filters = {.limit = 1, .kind = kind},
                                  .cursor = std::nullopt,
                                  .trusted_origin = ORIGIN},
                                 fixture.auth, "127.0.0.1",
                                 std::move(completion));
      });
      REQUIRE(valid);
      REQUIRE((*valid)["targets"].size() == 1);
      REQUIRE((*valid)["kind"].asString() ==
              (kind == frontend::SurfaceKind::PRIMARY ? "primary" : "tray"));
      REQUIRE((*valid)["targets"][0]["panel_id"].asString() ==
              (kind == frontend::SurfaceKind::PRIMARY ? "receiver" : "avatar"));
      REQUIRE_FALSE((*valid).isMember("admin_default_candidate"));
      if (kind == frontend::SurfaceKind::TRAY) {
        REQUIRE((*valid)["next_cursor"].isString());
        const auto cursor = (*valid)["next_cursor"].asString();
        const auto next = fixture.json([&](auto completion) {
          fixture.service->catalog({.filters = {.limit = 1, .kind = kind},
                                    .cursor = cursor,
                                    .trusted_origin = ORIGIN},
                                   fixture.auth, "127.0.0.1",
                                   std::move(completion));
        });
        REQUIRE(next);
        REQUIRE((*next)["targets"].size() == 1);
        REQUIRE((*next)["targets"][0]["panel_id"].asString() == "bell");
        REQUIRE((*next)["next_cursor"].isNull());
        const auto cross_kind = fixture.json([&](auto completion) {
          fixture.service->catalog(
              {.filters = {.limit = 1, .kind = frontend::SurfaceKind::PRIMARY},
               .cursor = cursor,
               .trusted_origin = ORIGIN},
              fixture.auth, "127.0.0.1", std::move(completion));
        });
        REQUIRE_FALSE(cross_kind);
        REQUIRE(cross_kind.error().code == "invalid_argument");
      }
      REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                  *fixture.service) == 0);
    }
  }
  SECTION(
      "primary and tray share captured authority without float capability") {
    using Kind = frontend::SurfaceKind;
    const auto typed = [](std::string panel, Kind kind) {
      return frontend::SurfaceLookup{.application_id = "floatprovider",
                                     .panel_id = std::move(panel),
                                     .kind = kind,
                                     .expected_generation = std::nullopt,
                                     .content_type = std::nullopt,
                                     .capability = std::nullopt};
    };
    fixture.parameters("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT "
                       "id FROM plinth.rbac_rules WHERE rule=$1)",
                       {"floatprovider.preview"});
    const auto float_denied = fixture.admit({lookup()});
    REQUIRE(float_denied);
    REQUIRE((*float_denied)["results"][0]["status"].asString() ==
            "unavailable");
    const auto result = fixture.admit(
        {typed("receiver", Kind::PRIMARY), typed("legacy", Kind::PRIMARY),
         typed("bell", Kind::TRAY), typed("avatar", Kind::TRAY)});
    REQUIRE(result);
    REQUIRE((*result)["results"].size() == 4);
    REQUIRE((*result)["results"][1]["status"].asString() == "unavailable");
    REQUIRE((*result)["results"][1].size() == 2);
    for (const Json::ArrayIndex index : {0U, 2U, 3U}) {
      const auto& row = (*result)["results"][index];
      REQUIRE(row["index"].asUInt() == index);
      REQUIRE(row["status"].asString() == "available");
      const auto& target = row["target"];
      REQUIRE_FALSE(target.isMember("capability"));
      REQUIRE_FALSE(target.isMember("logical_install_order"));
      REQUIRE_FALSE(target.isMember("persistence_contract"));
      if (index == 0) {
        REQUIRE(target["type"].asString() == "primary");
        REQUIRE(target["accepts_navigation_intent"].asBool());
      } else {
        REQUIRE(target["type"].asString() == "tray");
        REQUIRE_FALSE(target["chrome_essential"].asBool());
        REQUIRE_FALSE(target.isMember("context_schema"));
      }
      const auto selector = frontend::protocol::parse_asset_selector(
          target["module_url"].asString());
      REQUIRE(selector);
      const auto response =
          fixture.asset(*selector, fixture.auth, drogon::Head);
      REQUIRE(response);
      REQUIRE((*response)->statusCode() == drogon::k200OK);
    }
    auto stale = typed("receiver", Kind::PRIMARY);
    stale.expected_generation = fixture.provider.id;
    const auto current = fixture.admit({stale});
    REQUIRE(current);
    REQUIRE((*current)["results"][0]["status"].asString() == "available");
    stale.expected_generation = "00000000-0000-0000-0000-000000000046";
    const auto pinned = fixture.admit({stale});
    REQUIRE(pinned);
    REQUIRE((*pinned)["results"][0]["status"].asString() == "stale");
    fixture.parameters("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT "
                       "id FROM plinth.rbac_rules WHERE rule=$1)",
                       {"floatprovider.panel"});
    const auto denied = fixture.admit(
        {typed("receiver", Kind::PRIMARY), typed("bell", Kind::TRAY)});
    REQUIRE(denied);
    for (const auto& row : (*denied)["results"]) {
      REQUIRE(row["status"].asString() == "unavailable");
      REQUIRE(row.size() == 2);
    }
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

TEST_CASE("float layout service: native admin defaults fresh authority and "
          "exact storage",
          "[frontend][float_layout_service][admin_defaults][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  Fixture fixture;
  SECTION("nonadmin every management command refuses before fixed dispatch") {
    const auto before =
        frontend::test_seam::fixed_storage_dispatch_attempts(*fixture.service);
    for (const auto& result : {fixture.defaults_read(), fixture.defaults_put(),
                               fixture.defaults_delete()}) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().code == "permission_denied");
    }
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before);
    REQUIRE(fixture.scalar(
                "SELECT count(*)::text FROM ext_shell.default_apps") == "0");
  }
  SECTION("actual put read upsert delete absence and retained uninstalled "
          "logical handler") {
    fixture.grant_admin();
    {
      const auto absent = fixture.defaults_read(
          frontend::admin_defaults::ReadSingle{"text/plain"});
      REQUIRE(absent);
      REQUIRE(absent->prepare_handoff());
      REQUIRE(absent->profile() == fixture.platform->platform->profile());
      REQUIRE(absent->body().size() == 3);
      REQUIRE(absent->body()["schema_version"].asInt() == 1);
      REQUIRE(absent->body()["handler"].isNull());
    }
    {
      const auto saved = fixture.defaults_put();
      REQUIRE(saved);
      REQUIRE(saved->prepare_handoff());
      REQUIRE(saved->body().size() == 2);
      REQUIRE(saved->body()["saved"].asBool());
      REQUIRE(fixture.scalar(
                  "SELECT application_id || '/' || panel_id FROM "
                  "ext_shell.default_apps WHERE content_type='text/plain'") ==
              "uninstalled-app/old_panel");
    }
    {
      const auto saved = fixture.management([&](auto completion) {
        fixture.service->put_admin_default({"text/plain", {"another-app", "p"}},
                                           fixture.auth, "127.0.0.1", 0,
                                           std::move(completion));
      });
      REQUIRE(saved);
      REQUIRE(saved->body()["saved"].asBool());
    }
    {
      const auto single = fixture.defaults_read(
          frontend::admin_defaults::ReadSingle{"text/plain"});
      REQUIRE(single);
      REQUIRE(single->body()["handler"]["application_id"].asString() ==
              "another-app");
      REQUIRE(single->body()["handler"]["panel_id"].asString() == "p");
      REQUIRE(single->body()["handler"].size() == 2);
    }
    for (const bool expected : {true, false}) {
      const auto removed = fixture.defaults_delete();
      REQUIRE(removed);
      REQUIRE(removed->prepare_handoff());
      REQUIRE(removed->body().size() == 2);
      REQUIRE(removed->body()["deleted"].isBool());
      REQUIRE(removed->body()["deleted"].asBool() == expected);
    }
    REQUIRE(fixture.scalar(
                "SELECT count(*)::text FROM ext_shell.default_apps") == "0");
    REQUIRE(fixture.scalar(
                "SELECT count(*)::text FROM ext_shell.user_preferences") ==
            "0");
  }
  SECTION("fresh grant revoke expiry rotation and PAT use actual original "
          "credential") {
    fixture.grant_admin();
    REQUIRE(fixture.defaults_put());
    fixture.command("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT id "
                    "FROM plinth.rbac_rules WHERE rule='kernel.admin') AND "
                    "group_id=(SELECT id FROM plinth.groups WHERE "
                    "name='float_service_viewers')");
    auto before =
        frontend::test_seam::fixed_storage_dispatch_attempts(*fixture.service);
    const auto revoked_rule = fixture.defaults_read();
    REQUIRE_FALSE(revoked_rule);
    REQUIRE(revoked_rule.error().code == "permission_denied");
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before);
    fixture.grant_admin();
    fixture.parameters("UPDATE plinth.sessions SET expires_at=NOW()-INTERVAL "
                       "'1 second' WHERE id=$1::uuid",
                       {fixture.auth.session_id});
    const auto expired = fixture.defaults_delete();
    REQUIRE_FALSE(expired);
    REQUIRE(expired.error().code == "not_authenticated");
    fixture.parameters("UPDATE plinth.sessions SET expires_at=NOW()+INTERVAL "
                       "'1 hour', token_hash=$2 WHERE id=$1::uuid",
                       {fixture.auth.session_id, std::string(64, 'b')});
    const auto rotated = fixture.defaults_read();
    REQUIRE_FALSE(rotated);
    REQUIRE(rotated.error().code == "not_authenticated");
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before);
    // PAT creation is an ordinary test identity fixture; no management grant
    // or browser credential bypass is inferred from the caller's auth fields.
    const auto token = fixture.pg->exec_params(
        "INSERT INTO plinth.pats(user_id,name,token_hash,token_prefix) "
        "VALUES($1::uuid,'admin-service-test',$2,'plinth_test') RETURNING "
        "id::text",
        {fixture.auth.user_id, std::string(64, 'c')});
    REQUIRE(PQresultStatus(token.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(token.get()) == 1);
    fixture.auth.auth_type = "pat";
    fixture.auth.pat_id = PQgetvalue(token.get(), 0, 0);
    fixture.auth.session_id.clear();
    fixture.auth.token_hash = std::string(64, 'c');
    fixture.auth.credential_source = plinth::auth::CredentialSource::BEARER;
    REQUIRE(fixture.defaults_delete());
  }
  SECTION(
      "native grammar and original caller depth are checked before dispatch") {
    fixture.grant_admin();
    const auto before =
        frontend::test_seam::fixed_storage_dispatch_attempts(*fixture.service);
    for (const int depth : {-1, plinth::capabilities::MAX_CALL_DEPTH}) {
      const auto bad = fixture.defaults_put("text/plain", depth);
      REQUIRE_FALSE(bad);
      REQUIRE(bad.error().code == "invalid_argument");
    }
    const auto invalid = fixture.defaults_put("text/plain\n");
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == "invalid_argument");
    const auto invalid_page = fixture.defaults_read(
        frontend::admin_defaults::ReadPage{65, std::nullopt});
    REQUIRE_FALSE(invalid_page);
    REQUIRE(invalid_page.error().code == "invalid_argument");
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before);
    REQUIRE(fixture.defaults_put("text/plain", 2));
    REQUIRE(frontend::test_seam::admin_dispatch_caller_depth(
                *fixture.service) == 2);
    REQUIRE(fixture.defaults_read(
        frontend::admin_defaults::ReadSingle{"text/plain"},
        plinth::capabilities::MAX_CALL_DEPTH - 1));
    REQUIRE(frontend::test_seam::admin_dispatch_caller_depth(
                *fixture.service) == plinth::capabilities::MAX_CALL_DEPTH - 1);
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before + 2);
  }
  SECTION("64 row bytewise paging validates stored rows and credential-bound "
          "continuation") {
    fixture.grant_admin();
    // SQL seeds paging data only, not mutation acknowledgement evidence. The
    // preceding section exercises actual native PUT/upsert/DELETE completion.
    fixture.command(
        "INSERT INTO "
        "ext_shell.default_apps(content_type,application_id,panel_id) SELECT "
        "'application/x-' || n::text,'uninstalled-app','old_panel' FROM "
        "generate_series(100,164) AS n");
    std::string cursor;
    {
      const auto first = fixture.defaults_read();
      REQUIRE(first);
      REQUIRE(first->prepare_handoff());
      REQUIRE(first->body()["entries"].size() == 64);
      REQUIRE(first->body()["entries"][0]["content_type"].asString() ==
              "application/x-100");
      REQUIRE(first->body()["entries"][63]["content_type"].asString() ==
              "application/x-163");
      REQUIRE(
          first->body()["entries"][0]["handler"]["application_id"].asString() ==
          "uninstalled-app");
      REQUIRE(first->body()["next_cursor"].isString());
      REQUIRE_FALSE(first->body().isMember("next_after"));
      cursor = first->body()["next_cursor"].asString();
    }
    {
      const auto last =
          fixture.defaults_read(frontend::admin_defaults::ReadPage{64, cursor});
      REQUIRE(last);
      REQUIRE(last->body()["entries"].size() == 1);
      REQUIRE(last->body()["entries"][0]["content_type"].asString() ==
              "application/x-164");
      REQUIRE(last->body()["next_cursor"].isNull());
    }
    const auto before =
        frontend::test_seam::fixed_storage_dispatch_attempts(*fixture.service);
    const auto changed_limit =
        fixture.defaults_read(frontend::admin_defaults::ReadPage{1, cursor});
    REQUIRE_FALSE(changed_limit);
    REQUIRE(changed_limit.error().code == "invalid_argument");
    fixture.auth.credential_source = plinth::auth::CredentialSource::BEARER;
    const auto changed_source =
        fixture.defaults_read(frontend::admin_defaults::ReadPage{64, cursor});
    REQUIRE_FALSE(changed_source);
    REQUIRE(changed_source.error().code == "invalid_argument");
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before);
    fixture.auth.credential_source = plinth::auth::CredentialSource::COOKIE;
    // The module's JS '$' can accept this stored trailing newline; the exact
    // native validator must refuse the whole result, not return the good row.
    fixture.command("UPDATE ext_shell.default_apps SET "
                    "content_type=E'application/x-160\\n' WHERE "
                    "content_type='application/x-160'");
    const auto malformed = fixture.defaults_read();
    REQUIRE_FALSE(malformed);
    REQUIRE(malformed.error().code == "storage_unavailable");
  }
}

TEST_CASE("float layout service: admin native settlement and real lifecycle "
          "ACK fence",
          "[frontend][float_layout_service][admin_defaults][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  Fixture fixture;
  fixture.grant_admin();
  AdminPause pause{*fixture.service};
  plinth::lifecycle::AsyncTaskRegistry accepted;
  auto completion = std::make_shared<
      std::promise<frontend::FloatLayoutService::AdminResult>>();
  auto reply = completion->get_future();
  auto adapter_mutex = std::make_shared<std::mutex>();
  auto callback_adapter =
      std::make_shared<frontend::FloatLayoutService::AdminCompletion>();
  const auto before =
      frontend::test_seam::fixed_storage_dispatch_attempts(*fixture.service);
  fixture.service->put_admin_default(
      {"text/plain", {"uninstalled-app", "old_panel"}}, fixture.auth,
      "127.0.0.1", 0,
      [completion, adapter_mutex, callback_adapter](auto result) {
        frontend::FloatLayoutService::AdminCompletion adapter;
        {
          std::lock_guard lock{*adapter_mutex};
          adapter = std::move(*callback_adapter);
        }
        if (adapter)
          adapter(std::move(result));
        else
          completion->set_value(std::move(result));
      },
      accepted.try_acquire());
  REQUIRE(pause.wait_entered());
  REQUIRE(reply.wait_for(0ms) == std::future_status::timeout);
  REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
              *fixture.service) == before + 1);
  REQUIRE(fixture.scalar("SELECT application_id FROM ext_shell.default_apps "
                         "WHERE content_type='text/plain'") ==
          "uninstalled-app");
  SECTION("unchanged actual settlement control keeps admitted profile through "
          "handoff") {
    pause.gate->release();
    REQUIRE(reply.wait_for(5s) == std::future_status::ready);
    {
      const auto result = reply.get();
      REQUIRE(result);
      REQUIRE(result->body()["saved"].asBool());
      REQUIRE(result->profile() == fixture.platform->platform->profile());
      REQUIRE(result->prepare_handoff());
      REQUIRE(accepted.active_for_test() == 1);
      REQUIRE(fixture.service->active_count() == 1);
    }
    REQUIRE(accepted.drain(5s));
    REQUIRE(fixture.service->drain(std::chrono::steady_clock::now() + 5s));
  }
  SECTION("ordinary disable owns name and drain so final ACK refuses promptly "
          "without retry") {
    auto lifecycle_context = fixture.context;
    lifecycle_context.upgrade_drain_timeout_ms = 4s;
    auto disabled = std::make_shared<std::promise<bool>>();
    auto disabled_result = disabled->get_future();
    ReleaseAdminBeforeJoin attempt{
        pause.gate, std::jthread{[context = std::move(lifecycle_context),
                                  id = fixture.platform->id,
                                  disabled](std::stop_token stop) {
          try {
            plinth::db::OperationScope scope{
                stop, std::chrono::steady_clock::now() + 8s};
            disabled->set_value(
                packages::disable_package(id, context).has_value());
          } catch (...) {
            disabled->set_value(false);
          }
        }}};
    const auto fence_deadline = std::chrono::steady_clock::now() + 2s;
    while (!plinth::capabilities::drain::is_fenced("shell") &&
           std::chrono::steady_clock::now() < fence_deadline)
      std::this_thread::yield();
    REQUIRE(plinth::capabilities::drain::is_fenced("shell"));
    REQUIRE(disabled_result.wait_for(0ms) == std::future_status::timeout);
    const auto released = std::chrono::steady_clock::now();
    pause.gate->release();
    REQUIRE(reply.wait_for(2s) == std::future_status::ready);
    const auto result = reply.get();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == "storage_unavailable");
    REQUIRE(std::chrono::steady_clock::now() - released < 2s);
    REQUIRE(disabled_result.wait_for(5s) == std::future_status::ready);
    REQUIRE(disabled_result.get());
    attempt.thread.join();
    REQUIRE(accepted.drain(5s));
    REQUIRE(fixture.service->drain(std::chrono::steady_clock::now() + 5s));
    REQUIRE(fixture.scalar("SELECT application_id FROM ext_shell.default_apps "
                           "WHERE content_type='text/plain'") ==
            "uninstalled-app");
    // This is committed data plus refused server acknowledgement. It is not
    // SQL rollback, lost-COMMIT-ACK fault injection or client receipt proof.
  }
  SECTION("real retained worker success is refused by the distinct final route "
          "handoff fence") {
    pause.gate->release();
    REQUIRE(reply.wait_for(5s) == std::future_status::ready);
    RetainedAdminBeforeJoin retained{pause.gate, std::nullopt, {}};
    retained.result.emplace(reply.get());
    REQUIRE(retained.result->has_value());
    REQUIRE(retained.result->value().prepare_handoff());
    REQUIRE(retained.result->value().profile() ==
            fixture.platform->platform->profile());
    REQUIRE(retained.result->value().body()["saved"].asBool());
    REQUIRE(accepted.active_for_test() == 1);
    REQUIRE(fixture.service->active_count() == 1);
    auto lifecycle_context = fixture.context;
    lifecycle_context.upgrade_drain_timeout_ms = 4s;
    auto disabled = std::make_shared<std::promise<bool>>();
    auto disabled_result = disabled->get_future();
    retained.thread = std::jthread{[context = std::move(lifecycle_context),
                                    id = fixture.platform->id,
                                    disabled](std::stop_token stop) {
      try {
        plinth::db::OperationScope scope{stop,
                                         std::chrono::steady_clock::now() + 8s};
        disabled->set_value(packages::disable_package(id, context).has_value());
      } catch (...) {
        disabled->set_value(false);
      }
    }};
    const auto fence_deadline = std::chrono::steady_clock::now() + 2s;
    while (!plinth::capabilities::drain::is_fenced("shell") &&
           std::chrono::steady_clock::now() < fence_deadline)
      std::this_thread::yield();
    REQUIRE(plinth::capabilities::drain::is_fenced("shell"));
    REQUIRE(disabled_result.wait_for(0ms) == std::future_status::timeout);
    // Observe the ordinary lifecycle's already-established fence/counter; do
    // not replace the platform or construct an authority frame through it.
    const auto fence = plinth::capabilities::drain::begin_drain("shell");
    REQUIRE(fence->in_flight.load() >= 1);
    REQUIRE(accepted.active_for_test() == 1);
    REQUIRE(fixture.service->active_count() == 1);
    unsigned int calls = 0;
    drogon::HttpResponsePtr response;
    auto adapter = frontend::test_seam::float_admin_completion(
        [&](const auto& value) {
          ++calls;
          response = value;
        },
        true);
    adapter(std::move(*retained.result));
    retained.result.reset();
    REQUIRE(calls == 1);
    REQUIRE(response);
    REQUIRE(response->statusCode() == drogon::k503ServiceUnavailable);
    REQUIRE(response->getHeader("X-Plinth-Platform-Profile").empty());
    REQUIRE(response->body() == R"({"error":"service_unavailable"})");
    REQUIRE(response->getHeader("Cache-Control") == "no-store");
    REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
                *fixture.service) == before + 1);
    REQUIRE(disabled_result.wait_for(5s) == std::future_status::ready);
    REQUIRE(disabled_result.get());
    retained.thread.join();
    REQUIRE(accepted.drain(5s));
    REQUIRE(fixture.service->drain(std::chrono::steady_clock::now() + 5s));
    REQUIRE(fence->in_flight.load() == 0);
    REQUIRE(calls == 1);
    REQUIRE(fixture.scalar("SELECT application_id FROM ext_shell.default_apps "
                           "WHERE content_type='text/plain'") ==
            "uninstalled-app");
  }
  SECTION("unchanged real service completion passes the actual adapter with "
          "its exact admitted profile") {
    auto observed = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
    auto response_ready = observed->get_future();
    auto calls = std::make_shared<std::atomic<unsigned int>>(0);
    const auto admitted_profile = fixture.platform->platform->profile();
    auto adapter = frontend::test_seam::float_admin_completion(
        [observed, calls](const auto& response) {
          calls->fetch_add(1);
          observed->set_value(response);
        },
        true);
    {
      std::lock_guard lock{*adapter_mutex};
      *callback_adapter = adapter;
    }
    pause.gate->release();
    REQUIRE(response_ready.wait_for(5s) == std::future_status::ready);
    const auto response = response_ready.get();
    REQUIRE(response);
    REQUIRE(response->statusCode() == drogon::k200OK);
    REQUIRE(response->getHeader("X-Plinth-Platform-Profile") ==
            admitted_profile);
    REQUIRE(nlohmann::json::parse(response->body()) ==
            nlohmann::json{{"schema_version", 1}, {"saved", true}});
    REQUIRE(calls->load() == 1);
    REQUIRE(accepted.drain(5s));
    REQUIRE(fixture.service->drain(std::chrono::steady_clock::now() + 5s));
  }
  SECTION("throwing actual success adapter remains one-shot and sticky in the "
          "service completion") {
    fixture.expected_delivery_failure = true;
    auto observed = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
    auto response_ready = observed->get_future();
    auto calls = std::make_shared<std::atomic<unsigned int>>(0);
    auto adapter = frontend::test_seam::float_admin_completion(
        [observed, calls](const auto& response) {
          calls->fetch_add(1);
          observed->set_value(response);
          throw std::runtime_error{"controlled actual admin handoff callback"};
        },
        true);
    {
      std::lock_guard lock{*adapter_mutex};
      *callback_adapter = adapter;
    }
    pause.gate->release();
    REQUIRE(response_ready.wait_for(5s) == std::future_status::ready);
    const auto response = response_ready.get();
    REQUIRE(response->statusCode() == drogon::k200OK);
    REQUIRE(response->getHeader("X-Plinth-Platform-Profile") ==
            fixture.platform->platform->profile());
    REQUIRE(accepted.drain(5s));
    REQUIRE_FALSE(
        fixture.service->drain(std::chrono::steady_clock::now() + 5s));
    REQUIRE_FALSE(
        fixture.service->drain(std::chrono::steady_clock::now() + 5s));
    REQUIRE_NOTHROW(adapter(std::unexpected(
        plinth::js::PromiseRejection{.code = "storage_unavailable",
                                     .message = "controlled",
                                     .sqlstate = std::nullopt})));
    REQUIRE(calls->load() == 1);
  }
  SECTION("both production fences share the actual immutable identity mismatch "
          "predicate") {
    pause.gate->release();
    REQUIRE(reply.wait_for(5s) == std::future_status::ready);
    {
      const auto result = reply.get();
      REQUIRE(result);
      const auto storage = plinth::extensions::fixed_shell_storage_binding();
      REQUIRE(storage);
      const auto& platform = *fixture.platform->platform;
      const frontend::admin_ack_detail::FrameIdentity admitted{
          .platform_key = platform.key(),
          .profile = platform.profile(),
          .artifact_digest = platform.artifact_digest(),
          .storage_identity = storage->identity,
          .storage_artifact_digest = storage->artifact_digest};
      REQUIRE(frontend::admin_ack_detail::same_identity(admitted, admitted));
      for (unsigned int variant = 0; variant < 9; ++variant) {
        auto changed = admitted;
        if (variant == 0)
          changed.platform_key.package_id.back() =
              changed.platform_key.package_id.back() == '0' ? '1' : '0';
        if (variant == 1) changed.platform_key.version += ".changed";
        if (variant == 2)
          changed.artifact_digest[0] =
              changed.artifact_digest[0] == 'a' ? 'b' : 'a';
        if (variant == 3)
          changed.profile.back() = changed.profile.back() == 'a' ? 'b' : 'a';
        if (variant == 4) ++changed.storage_identity.serial;
        if (variant == 5)
          changed.storage_identity.package_id.back() =
              changed.storage_identity.package_id.back() == '0' ? '1' : '0';
        if (variant == 6) changed.storage_identity.version += ".changed";
        if (variant == 7)
          changed.storage_artifact_digest[0] =
              changed.storage_artifact_digest[0] == 'a' ? 'b' : 'a';
        if (variant == 8) ++changed.platform_key.profile;
        REQUIRE_FALSE(
            frontend::admin_ack_detail::same_identity(admitted, changed));
        REQUIRE_FALSE(
            frontend::admin_ack_detail::same_identity(changed, admitted));
      }
      REQUIRE(result->prepare_handoff());
      // Copied metadata comparisons do not publish a replacement or create a
      // success frame/guard; real identity stays unchanged under this ACK
      // owner.
    }
    REQUIRE(accepted.drain(5s));
    REQUIRE(fixture.service->drain(std::chrono::steady_clock::now() + 5s));
  }
  REQUIRE_FALSE(pause.expired());
  REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
              *fixture.service) == before + 1);
  REQUIRE(fixture.service->active_count() == 0);
}

TEST_CASE("float layout service: blocked admin SQL retains physical owner "
          "after logical cancellation",
          "[frontend][float_layout_service][admin_defaults][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  Fixture fixture;
  fixture.grant_admin();
  // This connection drops/rolls back before fixture drain even if an assertion
  // unwinds. It blocks actual bundled-module DML, not a simulated result.
  plinth::ws_test::TestPg blocker{fixture.config.db};
  {
    const auto locked = blocker.exec(
        "BEGIN; LOCK TABLE ext_shell.default_apps IN ACCESS EXCLUSIVE MODE");
    REQUIRE(PQresultStatus(locked.get()) == PGRES_COMMAND_OK);
  }
  plinth::lifecycle::AsyncTaskRegistry accepted;
  auto completion = std::make_shared<
      std::promise<frontend::FloatLayoutService::AdminResult>>();
  auto reply = completion->get_future();
  auto calls = std::make_shared<std::atomic<unsigned int>>(0);
  const auto before =
      frontend::test_seam::fixed_storage_dispatch_attempts(*fixture.service);
  fixture.service->put_admin_default(
      {"text/plain", {"uninstalled-app", "old_panel"}}, fixture.auth,
      "127.0.0.1", 0,
      [completion, calls](auto result) {
        calls->fetch_add(1);
        completion->set_value(std::move(result));
      },
      accepted.try_acquire());
  const auto observed_deadline = std::chrono::steady_clock::now() + 3s;
  bool waiting = false;
  while (!waiting && std::chrono::steady_clock::now() < observed_deadline) {
    waiting =
        fixture.scalar(
            "SELECT EXISTS(SELECT 1 FROM pg_locks WHERE locktype='relation' "
            "AND relation='ext_shell.default_apps'::regclass AND NOT "
            "granted)::text") == "true";
    if (!waiting) std::this_thread::yield();
  }
  REQUIRE(waiting);
  REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
              *fixture.service) == before + 1);
  REQUIRE(reply.wait_for(0ms) == std::future_status::timeout);
  REQUIRE_FALSE(
      fixture.service->drain(std::chrono::steady_clock::now() + 100ms));
  REQUIRE(reply.wait_for(2s) == std::future_status::ready);
  const auto cancelled = reply.get();
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code == "storage_unavailable");
  REQUIRE(calls->load() == 1);
  REQUIRE(fixture.service->active_count() == 1);
  REQUIRE(accepted.active_for_test() == 1);
  const auto fenced = plinth::capabilities::drain::begin_drain("shell");
  REQUIRE(fenced->in_flight.load() >= 1);
  REQUIRE_FALSE(accepted.drain(0ms));
  {
    const auto released = blocker.exec("ROLLBACK");
    REQUIRE(PQresultStatus(released.get()) == PGRES_COMMAND_OK);
  }
  REQUIRE(accepted.drain(5s));
  REQUIRE(fixture.service->drain(std::chrono::steady_clock::now() + 5s));
  REQUIRE(fixture.service->active_count() == 0);
  REQUIRE(fenced->in_flight.load() == 0);
  REQUIRE(calls->load() == 1);
  REQUIRE(frontend::test_seam::fixed_storage_dispatch_attempts(
              *fixture.service) == before + 1);
  REQUIRE(fixture.scalar("SELECT application_id FROM ext_shell.default_apps "
                         "WHERE content_type='text/plain'") ==
          "uninstalled-app");
  // The original mutation completed after logical503. This proves no retry
  // and retained native ownership, not rollback or lost SQL COMMIT-ACK.
}
