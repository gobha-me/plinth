#include "../ws/ws_test_fixture.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/frontend/platform_startup.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/installed_generation_registry.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/shell/firstboot.hpp"
#include "kernel/shell/platform_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace frontend = plinth::frontend;
namespace generation = plinth::packages::generation;
using namespace std::chrono_literals;

struct StartupTree {
  fs::path root;
  StartupTree() {
    std::array<char, 80> pattern{};
    std::ranges::copy(std::string_view{"/tmp/plinth-platform-startup.XXXXXX"},
                      pattern.begin());
    const char* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("startup test tree creation");
    root = created;
  }
  ~StartupTree() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  StartupTree(const StartupTree&) = delete;
  auto operator=(const StartupTree&) -> StartupTree& = delete;
};

struct Fixture {
  StartupTree tree;
  plinth::Config config{plinth::ws_test::test_config()};
  plinth::packages::InstallerContext context;
  std::shared_ptr<generation::InstalledGenerationRegistry> registry;
  std::shared_ptr<PGconn> connection;
  std::vector<std::pair<std::string, std::string>> owned_routes;
  Fixture() {
    plinth::ws_test::settle_fixture_work();
    static_cast<void>(plinth::extensions::shutdown_registry());
    plinth::shell::test_seam::set_active_frontend(std::nullopt);
    plinth::capabilities::drain::end_drain("shell");
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
    connection = std::shared_ptr<PGconn>{
        PQconnectdb(plinth::db::connection_info(config.db).c_str()), PQfinish};
    REQUIRE(connection);
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  }
  ~Fixture() {
    plinth::ws_test::settle_fixture_work_or_exit();
    for (const auto& [name, version] : owned_routes)
      plinth::packages::asset_server::unregister_routes(name, version);
    static_cast<void>(plinth::extensions::shutdown_registry());
    plinth::shell::test_seam::set_active_frontend(std::nullopt);
    plinth::capabilities::drain::end_drain("shell");
    registry->clear();
  }
  Fixture(const Fixture&) = delete;
  auto operator=(const Fixture&) -> Fixture& = delete;
  auto install() -> void {
    // Read the actual archive version before installation may publish a route;
    // do not guess it from VERSION or whichever row survives a failed install.
    const auto bundle = plinth::shell::bundled_shell_status(config);
    REQUIRE(bundle);
    owned_routes.emplace_back("shell", bundle->available_version);
    // Deliberately retain the legacy null-owner firstboot context so startup
    // must establish a real existing-generation receipt, not a forged fixture.
    REQUIRE(plinth::shell::ensure_bundled_shell_installed(config, context));
    REQUIRE(
        scalar("SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
  }
  auto command(const std::string& sql) -> void {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    REQUIRE(result);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto scalar(const std::string& sql) -> std::string {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    REQUIRE(result);
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    REQUIRE_FALSE(PQgetisnull(result.get(), 0, 0));
    return PQgetvalue(result.get(), 0, 0);
  }
  auto prepare() {
    return frontend::prepare_platform_startup(
        config, registry, std::chrono::steady_clock::now() + 10s);
  }
};
} // namespace

TEST_CASE("platform startup: exact bundled publication and explicit receipt "
          "ownership",
          "[frontend][platform_startup][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture fixture;
  SECTION("no frontend is affirmative absence without a receipt or graph") {
    const auto result = fixture.prepare();
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    REQUIRE_FALSE(plinth::shell::current_platform());
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
  }
  SECTION("custom frontend remains ordinary and is not approved by its name") {
    fixture.command(
        "INSERT INTO "
        "plinth.packages(name,version,state,provenance,manifest_json,entry_"
        "point,manifest_checksum,frontend_mount,frontend_entry) "
        "VALUES('customui','1.0.0','ACTIVE','user','{}','server/"
        "main.js',repeat('f',64),'/custom','index.html')");
    const auto result = fixture.prepare();
    REQUIRE(result);
    REQUIRE(*result);
    REQUIRE((**result).name == "customui");
    REQUIRE_FALSE((**result).platform);
    REQUIRE_FALSE(plinth::extensions::fixed_shell_storage_binding());
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
  }
  SECTION("real source migration and stamped pool publish once without "
          "replacement") {
    fixture.install();
    const auto before = plinth::extensions::pool_identity("shell");
    REQUIRE(before);
    const auto result = fixture.prepare();
    REQUIRE(result);
    REQUIRE(*result);
    const auto& active = **result;
    REQUIRE(active.platform);
    REQUIRE(active.bundled);
    REQUIRE(active.id == before->package_id);
    REQUIRE(active.platform == plinth::shell::current_platform());
    const auto binding = plinth::extensions::fixed_shell_storage_binding();
    REQUIRE(binding);
    REQUIRE(binding->identity == *before);
    REQUIRE(binding->artifact_digest == active.platform->artifact_digest());
    REQUIRE(active.platform->document().find(active.platform->base()) !=
            std::string::npos);
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "1");
    REQUIRE_FALSE(plinth::capabilities::drain::is_fenced("shell"));
    REQUIRE_FALSE(plinth::capabilities::drain::is_application_blocked("shell"));
    const auto again = fixture.prepare();
    REQUIRE(again);
    REQUIRE(*again);
    REQUIRE((**again).platform);
    REQUIRE(plinth::extensions::pool_identity("shell") == before);
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "1");
  }
  SECTION("existing block is not silently repaired or cleared") {
    fixture.install();
    plinth::capabilities::drain::block_application("shell");
    const auto result = fixture.prepare();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() ==
            frontend::PlatformStartupError::RECOVERY_REQUIRED);
    REQUIRE(plinth::capabilities::drain::is_application_blocked("shell"));
    REQUIRE_FALSE(plinth::shell::current_platform());
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
  }
  SECTION(
      "clean physical pressure leaves legacy readiness and no typed receipt") {
    fixture.install();
    generation::Budget budget{std::size_t{80} * 1024 * 1024};
    fixture.registry =
        std::make_shared<generation::InstalledGenerationRegistry>(
            fixture.context.data_dir, budget);
    auto held = budget.try_reserve(std::size_t{32} * 1024 * 1024);
    REQUIRE(held);
    const auto result = fixture.prepare();
    REQUIRE(result);
    REQUIRE(*result);
    REQUIRE_FALSE((**result).platform);
    REQUIRE_FALSE(plinth::extensions::fixed_shell_storage_binding());
    REQUIRE_FALSE(plinth::capabilities::drain::is_fenced("shell"));
    REQUIRE_FALSE(plinth::capabilities::drain::is_application_blocked("shell"));
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE name='shell'") ==
            "ACTIVE");
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
  }
  SECTION("actual migration acknowledgement mismatch never publishes profile") {
    fixture.install();
    fixture.command("UPDATE plinth.migrations SET checksum=repeat('0',64) "
                    "WHERE extension_name='shell' AND "
                    "migration_file='002_platform_storage_v1.sql'");
    const auto result = fixture.prepare();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == frontend::PlatformStartupError::UNAVAILABLE);
    REQUIRE_FALSE(plinth::shell::current_platform());
    REQUIRE_FALSE(plinth::extensions::fixed_shell_storage_binding());
    REQUIRE(plinth::capabilities::drain::is_application_blocked("shell"));
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE name='shell'") ==
            "ACTIVE");
  }
  SECTION(
      "unsupported client graph remains ordinary without profile approval") {
    fixture.install();
    const auto version = fixture.scalar(
        "SELECT version FROM plinth.packages WHERE name='shell'");
    {
      std::ofstream changed{fixture.context.data_dir / "extensions" / "shell" /
                                version / "client" / "shell.js",
                            std::ios::binary | std::ios::app};
      changed << "\n// Controlled unsupported bootstrap graph.\n";
      REQUIRE(changed.good());
    }
    const auto result = fixture.prepare();
    REQUIRE(result);
    REQUIRE(*result);
    REQUIRE_FALSE((**result).platform);
    REQUIRE_FALSE(plinth::shell::current_platform());
    REQUIRE_FALSE(plinth::extensions::fixed_shell_storage_binding());
    REQUIRE_FALSE(plinth::capabilities::drain::is_fenced("shell"));
    REQUIRE_FALSE(plinth::capabilities::drain::is_application_blocked("shell"));
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE name='shell'") ==
            "ACTIVE");
  }
  SECTION(
      "receipt SQL failure rolls back without changing existing ACTIVE state") {
    fixture.install();
    fixture.command("CREATE FUNCTION plinth.reject_startup_receipt() RETURNS "
                    "trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION "
                    "'controlled startup failure'; END $$");
    fixture.command("CREATE TRIGGER reject_startup_receipt BEFORE INSERT ON "
                    "plinth.package_artifact_receipts FOR EACH ROW EXECUTE "
                    "FUNCTION plinth.reject_startup_receipt()");
    const auto result = fixture.prepare();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == frontend::PlatformStartupError::UNAVAILABLE);
    REQUIRE_FALSE(plinth::shell::current_platform());
    REQUIRE_FALSE(plinth::extensions::fixed_shell_storage_binding());
    REQUIRE(plinth::capabilities::drain::is_application_blocked("shell"));
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE name='shell'") ==
            "ACTIVE");
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
  }
  SECTION(
      "actual missing runtime cannot be replaced by graph self attestation") {
    fixture.install();
    plinth::extensions::destroy_pool("shell");
    const auto result = fixture.prepare();
    REQUIRE_FALSE(result);
    REQUIRE_FALSE(plinth::shell::current_platform());
    REQUIRE_FALSE(plinth::extensions::fixed_shell_storage_binding());
    REQUIRE(plinth::capabilities::drain::is_application_blocked("shell"));
  }
  SECTION("original stop or expired deadline never bootstraps a receipt") {
    fixture.install();
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = frontend::prepare_platform_startup(
        fixture.config, fixture.registry,
        std::chrono::steady_clock::now() + 10s, stop.get_token());
    REQUIRE_FALSE(cancelled);
    REQUIRE(cancelled.error() == frontend::PlatformStartupError::INTERRUPTED);
    const auto expired = frontend::prepare_platform_startup(
        fixture.config, fixture.registry,
        std::chrono::steady_clock::now() - 1s);
    REQUIRE_FALSE(expired);
    REQUIRE(expired.error() == frontend::PlatformStartupError::INTERRUPTED);
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
    REQUIRE_FALSE(plinth::shell::current_platform());
  }
}
