#include "kernel/frontend/platform_startup.hpp"

#include "kernel/capabilities/drain.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/packages/installed_generation_registry.hpp"
#include "kernel/shell/platform_graph.hpp"

#include <plinth/platform_profile.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace plinth::frontend {
namespace {
namespace generation = packages::generation;
using Error = PlatformStartupError;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
using Active = std::optional<shell::ActiveFrontend>;

auto clean(generation::InstalledGenerationError error) -> bool {
  return error == generation::InstalledGenerationError::BUSY ||
         error == generation::InstalledGenerationError::PROFILE_UNAVAILABLE;
}

auto resolve(PGconn& connection, const Config& config)
    -> std::expected<Active, Error> {
  // CASE bounds text BEFORE libpq materializes it. Null means unsupported or
  // corrupt metadata, never an empty frontend or authority outage fallback.
  const Result result{
      db::exec(
          &connection,
          "SELECT id::text,CASE WHEN octet_length(name)<=64 THEN name END,"
          "CASE WHEN octet_length(version)<=256 THEN version END,"
          "CASE WHEN octet_length(frontend_mount)<=256 THEN frontend_mount END,"
          "CASE WHEN octet_length(frontend_entry)<=1024 THEN frontend_entry "
          "END,"
          "provenance='bundled' FROM plinth.packages "
          "WHERE frontend_mount IS NOT NULL AND state IN "
          "('ACTIVE','ACTIVE_FLAGGED') LIMIT 2"),
      PQclear};
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) > 1 || PQnfields(result.get()) != 6)
    return std::unexpected(Error::UNAVAILABLE);
  if (PQntuples(result.get()) == 0) return Active{};
  for (int column = 0; column < 6; ++column)
    if (PQgetisnull(result.get(), 0, column))
      return std::unexpected(Error::UNAVAILABLE);
  shell::ActiveFrontend active{
      .id = PQgetvalue(result.get(), 0, 0),
      .name = PQgetvalue(result.get(), 0, 1),
      .version = PQgetvalue(result.get(), 0, 2),
      .mount = PQgetvalue(result.get(), 0, 3),
      .entry = PQgetvalue(result.get(), 0, 4),
      .client_dir = {},
      .bundled = std::string_view{PQgetvalue(result.get(), 0, 5)} == "t"};
  active.client_dir = std::filesystem::path{config.packages_data_dir} /
                      "extensions" / active.name / active.version / "client";
  return Active{std::move(active)};
}

struct Fence {
  std::string name;
  bool finished = false;
  ~Fence() {
    if (!finished) capabilities::drain::block_application(name);
  }
  auto finish() -> void {
    capabilities::drain::end_drain(name);
    finished = true;
  }
};

struct Transaction {
  PGconn& connection;
  bool active = false;
  bool commit_attempted = false;
  ~Transaction() {
    // Do not manufacture a rollback/known outcome after an uncertain COMMIT.
    const auto status = PQtransactionStatus(&connection);
    if (active && !commit_attempted && PQstatus(&connection) == CONNECTION_OK &&
        (status == PQTRANS_INTRANS || status == PQTRANS_INERROR))
      static_cast<void>(Result{db::exec(&connection, "ROLLBACK"), PQclear});
  }
};

auto command(PGconn& connection, const char* sql) -> bool {
  const Result result{db::exec(&connection, sql), PQclear};
  return result && PQresultStatus(result.get()) == PGRES_COMMAND_OK &&
         std::string_view{PQcmdStatus(result.get())} == sql;
}

} // namespace

auto prepare_platform_startup(
    const Config& config,
    const std::shared_ptr<generation::InstalledGenerationRegistry>& registry,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token cancellation) -> std::expected<Active, Error> try {
  if (!registry) return std::unexpected(Error::UNAVAILABLE);
  db::OperationScope scope{cancellation, deadline};
  scope.checkpoint();
  auto connection = std::shared_ptr<PGconn>{
      db::connect(db::connection_info(config.db).c_str()), PQfinish};
  if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
    return std::unexpected(Error::UNAVAILABLE);
  auto selected = resolve(*connection, config);
  if (!selected || !*selected) return selected;
  auto& active = **selected;
  if (!active.bundled || active.name != "shell" || active.mount != "/app" ||
      active.entry != "index.html")
    return selected;

  auto operation =
      packages::name_operation::NameOperation::acquire(connection, active.name);
  if (!operation) return std::unexpected(Error::UNAVAILABLE);
  // Never release an existing unknown/lifecycle block as a side effect of
  // optional profile preparation. Its owning recovery protocol must repair it.
  if (capabilities::drain::is_fenced(active.name) ||
      capabilities::drain::is_application_blocked(active.name))
    return std::unexpected(Error::RECOVERY_REQUIRED);
  auto state = capabilities::drain::begin_drain(active.name);
  Fence fence{active.name};
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return std::unexpected(Error::INTERRUPTED);
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (!capabilities::drain::wait_for_zero(state, remaining).first)
    return std::unexpected(Error::UNAVAILABLE);
  scope.checkpoint();
  auto fresh = resolve(*connection, config);
  if (!fresh || !*fresh || (**fresh).id != active.id ||
      (**fresh).name != active.name || (**fresh).version != active.version ||
      !(**fresh).bundled || (**fresh).mount != active.mount ||
      (**fresh).entry != active.entry)
    return std::unexpected(Error::UNAVAILABLE);
  const generation::GenerationKey key{.package_id = active.id,
                                      .name = active.name,
                                      .version = active.version,
                                      .profile = 1};
  auto captured =
      registry->current_locked(*operation, key, deadline, cancellation);
  if (!captured && captured.error() ==
                       generation::InstalledGenerationError::RECEIPT_REQUIRED) {
    Transaction transaction{*connection};
    if (!command(*connection, "BEGIN"))
      return std::unexpected(Error::UNAVAILABLE);
    transaction.active = true;
    auto prepared =
        registry->stage_locked(*operation, key, deadline, cancellation);
    if (!prepared) {
      if (clean(prepared.error()) &&
          PQstatus(connection.get()) == CONNECTION_OK &&
          PQtransactionStatus(connection.get()) == PQTRANS_INTRANS) {
        scope.checkpoint();
        if (!command(*connection, "ROLLBACK"))
          return std::unexpected(Error::UNAVAILABLE);
        transaction.active = false;
        fence.finish();
        return selected;
      }
      return std::unexpected(Error::UNAVAILABLE);
    }
    transaction.commit_attempted = true;
    if (!command(*connection, "COMMIT"))
      return std::unexpected(Error::RECOVERY_REQUIRED);
    transaction.active = false;
    auto published = registry->publish_committed_locked(*operation, *prepared,
                                                        deadline, cancellation);
    if (!published) {
      if (clean(published.error()) &&
          PQstatus(connection.get()) == CONNECTION_OK &&
          PQtransactionStatus(connection.get()) == PQTRANS_IDLE) {
        scope.checkpoint();
        fence.finish();
        return selected;
      }
      return std::unexpected(Error::UNAVAILABLE);
    }
    captured = prepared->artifact();
  }
  if (!captured) {
    if (clean(captured.error()) &&
        PQstatus(connection.get()) == CONNECTION_OK &&
        PQtransactionStatus(connection.get()) == PQTRANS_IDLE) {
      scope.checkpoint();
      fence.finish();
      return selected;
    }
    return std::unexpected(Error::UNAVAILABLE);
  }

  auto budget = registry->source_budget();
  const auto graph = shell::prepare_platform(**captured, budget);
  if (!graph) {
    if (graph.error() == shell::PlatformGraphError::INCOMPATIBLE ||
        graph.error() == shell::PlatformGraphError::TOO_LARGE ||
        graph.error() == shell::PlatformGraphError::BUSY) {
      scope.checkpoint();
      fence.finish();
      return selected;
    }
    return std::unexpected(Error::UNAVAILABLE);
  }
  const Result migration{
      db::exec(connection.get(),
               "SELECT CASE WHEN octet_length(checksum)=64 THEN checksum END "
               "FROM plinth.migrations WHERE extension_name='shell' "
               "AND migration_file='002_platform_storage_v1.sql' LIMIT 2"),
      PQclear};
  if (!migration || PQresultStatus(migration.get()) != PGRES_TUPLES_OK ||
      PQntuples(migration.get()) != 1 || PQgetisnull(migration.get(), 0, 0))
    return std::unexpected(Error::UNAVAILABLE);
  const auto identity = extensions::pool_identity("shell");
  if (!identity || identity->package_id != active.id ||
      identity->version != active.version)
    return std::unexpected(Error::UNAVAILABLE);
  const extensions::FixedShellStorageDigests pins{
      .module = std::string{shell::platform_profile::MODULE_SHA256},
      .contract = std::string{shell::platform_profile::CONTRACT_SHA256},
      .migration = std::string{shell::platform_profile::MIGRATION_SHA256},
      .applied_migration = PQgetvalue(migration.get(), 0, 0)};
  auto fixed = extensions::prepare_fixed_shell_storage(**captured, *identity,
                                                       pins, budget);
  if (!fixed) return std::unexpected(Error::UNAVAILABLE);
  scope.checkpoint();
  if (!extensions::publish_fixed_shell_storage(std::move(*fixed)))
    return std::unexpected(Error::UNAVAILABLE);
  active.platform = *graph;
  if (!shell::publish_platform_frontend(active, *graph))
    return std::unexpected(Error::UNAVAILABLE);
  const auto binding = extensions::fixed_shell_storage_binding();
  if (!binding || binding->identity != *identity ||
      binding->artifact_digest != (*graph)->artifact_digest())
    return std::unexpected(Error::UNAVAILABLE);
  scope.checkpoint();
  if (!operation->release_checked()) return std::unexpected(Error::UNAVAILABLE);
  fence.finish();
  return selected;
} catch (const std::exception&) {
  return std::unexpected(Error::INTERRUPTED);
}

} // namespace plinth::frontend
