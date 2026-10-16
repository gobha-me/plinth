#include "../ws/ws_test_fixture.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/installed_generation_registry.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "typed_surface_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace packages = plinth::packages;
namespace generation = packages::generation;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Existing synchronous post-COMMIT test seam; no callback escapes upgrade().
struct SwapObstruction;
std::atomic<SwapObstruction*> swap_obstruction{nullptr};
struct SwapObstruction {
  fs::path active;
  bool installed = false;
  explicit SwapObstruction(fs::path path) : active(std::move(path)) {
    swap_obstruction.store(this);
    packages::test_seam::set_upgrade_swap_committed_hook([]() noexcept {
      auto* owner = swap_obstruction.load();
      if (!owner) return;
      std::error_code error;
      fs::remove(owner->active, error);
      if (!error) owner->installed = fs::create_directory(owner->active, error);
    });
  }
  ~SwapObstruction() {
    packages::test_seam::clear_upgrade_swap_committed_hook();
    swap_obstruction.store(nullptr);
  }
  SwapObstruction(const SwapObstruction&) = delete;
  auto operator=(const SwapObstruction&) -> SwapObstruction& = delete;
};

struct LifecycleTree {
  fs::path root;
  LifecycleTree() {
    std::array<char, 80> pattern{};
    std::ranges::copy(
        std::string_view{"/tmp/plinth-generation-lifecycle.XXXXXX"},
        pattern.begin());
    const char* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("lifecycle test tree creation");
    root = created;
  }
  ~LifecycleTree() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  LifecycleTree(const LifecycleTree&) = delete;
  auto operator=(const LifecycleTree&) -> LifecycleTree& = delete;
};

struct Fixture {
  LifecycleTree tree;
  plinth::Config config{plinth::ws_test::test_config()};
  packages::InstallerContext context;
  std::shared_ptr<PGconn> connection;
  plinth::typed_surface_test::NotesFenceCleanup fence;
  std::vector<std::pair<std::string, std::string>> owned_routes;

  Fixture() {
    plinth::ws_test::settle_fixture_work();
    static_cast<void>(plinth::extensions::shutdown_registry());
    plinth::ws_test::reset_schema(config.db);
    context.db = config.db;
    context.data_dir = tree.root / "data";
    context.staging_dir = tree.root / "staging";
    context.schedule_rbac_tests = false;
    fs::create_directories(context.data_dir);
    fs::create_directories(context.staging_dir);
    config.packages_data_dir = context.data_dir.string();
    config.packages_staging_dir = context.staging_dir.string();
    REQUIRE(plinth::capabilities::init_resolver(config.db));
    plinth::extensions::init_registry(config);
    REQUIRE(packages::rbac_test::start_async_workers());
    connection = std::shared_ptr<PGconn>{
        PQconnectdb(plinth::db::connection_info(config.db).c_str()), PQfinish};
    REQUIRE(connection);
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  }
  ~Fixture() {
    plinth::ws_test::settle_fixture_work_or_exit();
    // Only this fixture's installed/attempted generations belong to us. The
    // process-wide shutdown hook permanently seals unrelated asset dispatch.
    for (const auto& [name, version] : owned_routes)
      packages::asset_server::unregister_routes(name, version);
    static_cast<void>(plinth::extensions::shutdown_registry());
    if (context.generation_registry) context.generation_registry->clear();
  }
  Fixture(const Fixture&) = delete;
  auto operator=(const Fixture&) -> Fixture& = delete;

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
  auto install(std::string_view version = "1.2.3")
      -> std::expected<packages::PackageRecord, packages::InstallFailure> {
    // Remember before installation can publish a route and then fail.
    owned_routes.emplace_back("notes", version);
    const auto bytes = plinth::typed_surface_test::package(
        plinth::typed_surface_test::panels(
            {plinth::typed_surface_test::floating()}),
        version);
    return packages::install_package(bytes, packages::Provenance::USER,
                                     context);
  }
  auto current(const packages::PackageRecord& record)
      -> std::expected<std::shared_ptr<const generation::CapturedArtifact>,
                       generation::InstalledGenerationError> {
    auto lock = packages::name_operation::NameOperation::acquire(connection,
                                                                 record.name);
    REQUIRE(lock);
    return context.generation_registry->current_locked(
        *lock,
        {.package_id = record.id,
         .name = record.name,
         .version = record.version},
        std::chrono::steady_clock::now() + 10s);
  }
};
} // namespace

TEST_CASE("generation lifecycle: optional receipts follow acknowledged package "
          "cutover",
          "[packages][generation_lifecycle][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture fixture;
  SECTION("null owner preserves ordinary typed install without eager receipt") {
    const auto installed = fixture.install();
    REQUIRE(installed);
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
    const auto identity = plinth::extensions::pool_identity("notes");
    REQUIRE(identity);
    REQUIRE(identity->package_id == installed->id);
    REQUIRE(identity->version == installed->version);
  }
  SECTION("real install disable enable upgrade GC and uninstall retain "
          "external bytes") {
    fixture.context.generation_registry =
        std::make_shared<generation::InstalledGenerationRegistry>(
            fixture.context.data_dir);
    const auto installed = fixture.install();
    REQUIRE(installed);
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "1");
    REQUIRE(fixture.scalar("SELECT bool_and(logical_install_order>0)::text "
                           "FROM plinth.panels") == "true");
    auto captured = fixture.current(*installed);
    REQUIRE(captured);
    REQUIRE((*captured)->find("client/panels/editor.js"));
    const auto initial = plinth::extensions::pool_identity("notes");
    REQUIRE(initial);
    REQUIRE(packages::disable_package(installed->id, fixture.context));
    const auto inactive = fixture.current(*installed);
    REQUIRE_FALSE(inactive);
    REQUIRE(inactive.error() == generation::InstalledGenerationError::MISMATCH);
    REQUIRE((*captured)->find("manifest.json"));
    REQUIRE(packages::enable_package(installed->id, fixture.context));
    const auto enabled = plinth::extensions::pool_identity("notes");
    REQUIRE(enabled);
    REQUIRE(enabled->serial != initial->serial);
    REQUIRE(enabled->package_id == installed->id);
    REQUIRE(fixture.current(*installed));
    const auto bytes = plinth::typed_surface_test::package(
        plinth::typed_surface_test::panels(
            {plinth::typed_surface_test::floating()}),
        "1.2.4");
    fixture.owned_routes.emplace_back(installed->name, "1.2.4");
    const auto upgraded =
        packages::upgrade_package(bytes, installed->id, fixture.context);
    REQUIRE(upgraded);
    REQUIRE(upgraded->new_record.id != installed->id);
    const auto active = plinth::extensions::pool_identity("notes");
    REQUIRE(active);
    REQUIRE(active->package_id == upgraded->new_record.id);
    REQUIRE(active->version == "1.2.4");
    auto successor = fixture.current(upgraded->new_record);
    REQUIRE(successor);
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "2");
    fixture.command("UPDATE plinth.packages SET retired_at=NOW()-interval '2 "
                    "hours' WHERE state='SUPERSEDED'");
    const auto collected =
        packages::garbage_collect_superseded_versions(1h, fixture.context);
    REQUIRE(collected);
    REQUIRE(collected->collected_ids ==
            std::vector<std::string>{installed->id});
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "1");
    REQUIRE(fixture.current(upgraded->new_record));
    REQUIRE((*captured)->find("manifest.json"));
    REQUIRE(packages::uninstall_package(upgraded->new_record.id, true,
                                        fixture.context));
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
    REQUIRE((*successor)->find("manifest.json"));
  }
  SECTION(
      "clean physical pressure permits legacy activation without a receipt") {
    generation::Budget budget{std::size_t{80} * 1024 * 1024};
    fixture.context.generation_registry =
        std::make_shared<generation::InstalledGenerationRegistry>(
            fixture.context.data_dir, budget);
    auto held = budget.try_reserve(std::size_t{32} * 1024 * 1024);
    REQUIRE(held);
    const auto installed = fixture.install();
    REQUIRE(installed);
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE name='notes'") ==
            "ACTIVE");
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
    held.reset();
    const auto missing = fixture.current(*installed);
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error() ==
            generation::InstalledGenerationError::RECEIPT_REQUIRED);
  }
  SECTION("recovery stages only an actually promoted typed generation") {
    const auto installed = fixture.install();
    REQUIRE(installed);
    fixture.context.generation_registry =
        std::make_shared<generation::InstalledGenerationRegistry>(
            fixture.context.data_dir);
    REQUIRE(packages::reconcile_in_flight_installs(fixture.context));
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
    REQUIRE(fixture.scalar("SELECT application_ready::text FROM "
                           "plinth.packages WHERE name='notes'") == "true");
    // The actual startup readiness sweep must preserve ordinary readiness
    // without eagerly capturing all existing ACTIVE generations or inserting
    // a receipt merely because the optional registry has now been supplied.
    const auto readiness =
        packages::reconcile_application_readiness(fixture.context);
    REQUIRE(readiness);
    REQUIRE(*readiness == 0);
    REQUIRE(fixture.scalar("SELECT application_ready::text FROM "
                           "plinth.packages WHERE name='notes'") == "true");
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
    // Controlled persisted interrupted-activation state, with real extracted
    // package bytes and registration; no synthetic receipt/digest is supplied.
    fixture.command(
        "UPDATE plinth.packages SET state='ACTIVATING',application_ready=false "
        "WHERE name='notes'");
    REQUIRE(packages::reconcile_in_flight_installs(fixture.context));
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE name='notes'") ==
            "ACTIVE");
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "1");
    REQUIRE(fixture.current(*installed));
    const auto identity = plinth::extensions::pool_identity("notes");
    REQUIRE(identity);
    REQUIRE(identity->package_id == installed->id);
    REQUIRE(identity->version == installed->version);
  }
  SECTION("post ACK filesystem failure preserves committed successor and "
          "fences admission") {
    fixture.context.generation_registry =
        std::make_shared<generation::InstalledGenerationRegistry>(
            fixture.context.data_dir);
    const auto installed = fixture.install();
    REQUIRE(installed);
    auto retained = fixture.current(*installed);
    REQUIRE(retained);
    SwapObstruction obstruction{fixture.context.data_dir / "extensions" /
                                "notes" / "active"};
    const auto bytes = plinth::typed_surface_test::package(
        plinth::typed_surface_test::panels(
            {plinth::typed_surface_test::floating()}),
        "1.2.4");
    fixture.owned_routes.emplace_back(installed->name, "1.2.4");
    const auto upgraded =
        packages::upgrade_package(bytes, installed->id, fixture.context);
    REQUIRE(obstruction.installed);
    REQUIRE_FALSE(upgraded);
    REQUIRE(upgraded.error().report.at("committed") == true);
    REQUIRE(upgraded.error().report.at("commit_outcome") == "acknowledged");
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE version='1.2.4'") ==
            "ACTIVE");
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE version='1.2.3'") ==
            "SUPERSEDED");
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "2");
    REQUIRE(fs::is_directory(fixture.context.data_dir / "extensions" / "notes" /
                             "1.2.4"));
    REQUIRE((*retained)->find("manifest.json"));
    const plinth::capabilities::drain::DispatchGuard guard{"notes"};
    REQUIRE_FALSE(guard.admitted());
  }
  SECTION("receipt SQL failure is never treated as clean profile pressure") {
    fixture.context.generation_registry =
        std::make_shared<generation::InstalledGenerationRegistry>(
            fixture.context.data_dir);
    fixture.command("CREATE FUNCTION plinth.reject_test_receipt() RETURNS "
                    "trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION "
                    "'controlled receipt failure'; END $$");
    fixture.command("CREATE TRIGGER reject_test_receipt BEFORE INSERT ON "
                    "plinth.package_artifact_receipts FOR EACH ROW EXECUTE "
                    "FUNCTION plinth.reject_test_receipt()");
    const auto installed = fixture.install();
    REQUIRE_FALSE(installed);
    REQUIRE(installed.error().failed_at == packages::InstallStage::ACTIVATING);
    REQUIRE(fixture.scalar(
                "SELECT state FROM plinth.packages WHERE name='notes'") ==
            "INSTALL_FAILED");
    REQUIRE(
        fixture.scalar(
            "SELECT count(*)::text FROM plinth.package_artifact_receipts") ==
        "0");
    REQUIRE_FALSE(plinth::extensions::pool_identity("notes"));
    const plinth::capabilities::drain::DispatchGuard guard{"notes"};
    REQUIRE_FALSE(guard.admitted());
  }
}
