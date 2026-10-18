#include "kernel/frontend/float_layout_service.hpp"

#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/parser.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/frontend/owned_frontend_worker.hpp"
#include "kernel/frontend/surface_authority.hpp"
#include "kernel/frontend/typed_assets.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/installed_generation_registry.hpp"
#include "kernel/shell/active_frontend.hpp"
#include "kernel/shell/platform_graph.hpp"

#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/writer.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <plinth/platform_profile.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <variant>

namespace plinth::frontend {
namespace {

constexpr std::size_t MAX_PHYSICAL_REQUESTS = 64;
constexpr std::size_t REQUEST_COPY_BYTES = std::size_t{128} * 1024;
constexpr std::size_t ACTIVE_COPY_BYTES = std::size_t{16} * 1024 * 1024;

auto kind_name(SurfaceKind kind) -> std::string_view {
  switch (kind) {
    case SurfaceKind::PRIMARY: return "primary";
    case SurfaceKind::FLOAT: return "float";
    case SurfaceKind::TRAY: return "tray";
  }
  return {};
}

auto valid_catalog_filters(const protocol::CursorQuery& filters) -> bool {
  return !kind_name(filters.kind).empty() && filters.limit >= 1 &&
         filters.limit <= 64 &&
         (!filters.content_type ||
          (filters.kind == SurfaceKind::FLOAT &&
           is_canonical_content_type(*filters.content_type))) &&
         (!filters.include_admin_default ||
          (filters.kind == SurfaceKind::FLOAT && filters.content_type));
}

auto unavailable(std::string_view code = "storage_unavailable")
    -> js::PromiseRejection {
  return {.code = std::string{code},
          .message = "guarded shell storage unavailable",
          .sqlstate = std::nullopt};
}

auto authority_error(authority::Error error) -> js::PromiseRejection {
  switch (error) {
    case authority::Error::INVALID: return unavailable("invalid_argument");
    case authority::Error::UNAUTHENTICATED:
      return unavailable("not_authenticated");
    case authority::Error::DENIED: return unavailable("permission_denied");
    case authority::Error::UNAVAILABLE: return unavailable();
  }
  return unavailable();
}

auto jsoncpp(const nlohmann::json& value) -> Json::Value {
  const auto text = value.dump();
  Json::CharReaderBuilder builder;
  Json::Value output;
  std::string errors;
  const std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
  if (!reader->parse(text.data(), text.data() + text.size(), &output, &errors))
    throw std::runtime_error("bounded frontend serialization failed");
  return output;
}

auto unix_seconds() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

auto current_frontend_matches(PGconn& connection,
                              const shell::PreparedPlatform& platform) -> bool {
  const std::unique_ptr<PGresult, decltype(&PQclear)> result(
      db::exec(&connection,
               "SELECT "
               "id::text,name,version,provenance,frontend_mount,frontend_entry "
               "FROM plinth.packages WHERE frontend_mount IS NOT NULL "
               "AND state IN ('ACTIVE','ACTIVE_FLAGGED') LIMIT 2"),
      PQclear);
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) != 1 || PQnfields(result.get()) != 6)
    return false;
  for (int column = 0; column < 6; ++column)
    if (PQgetisnull(result.get(), 0, column)) return false;
  return platform.key().package_id == PQgetvalue(result.get(), 0, 0) &&
         std::string_view{PQgetvalue(result.get(), 0, 1)} == "shell" &&
         platform.key().version == PQgetvalue(result.get(), 0, 2) &&
         std::string_view{PQgetvalue(result.get(), 0, 3)} == "bundled" &&
         std::string_view{PQgetvalue(result.get(), 0, 4)} == "/app" &&
         std::string_view{PQgetvalue(result.get(), 0, 5)} == "index.html";
}

} // namespace

struct FloatLayoutService::State {
  State(
      Config cfg,
      std::shared_ptr<packages::generation::InstalledGenerationRegistry> source,
      std::shared_ptr<const packages::restore::ProtectedPolicy>
          protected_policy,
      std::shared_ptr<TypedAssetStreams> asset_streams)
      : config(std::move(cfg)),
        registry(source
                     ? std::move(source)
                     : std::make_shared<
                           packages::generation::InstalledGenerationRegistry>(
                           config.packages_data_dir)),
        policy(std::move(protected_policy)),
        streams(asset_streams ? std::move(asset_streams)
                              : std::make_shared<TypedAssetStreams>(
                                    registry->source_budget())) {}
  Config config;
  std::shared_ptr<packages::generation::InstalledGenerationRegistry> registry;
  std::shared_ptr<const packages::restore::ProtectedPolicy> policy;
  std::shared_ptr<TypedAssetStreams> streams;
  protocol::CursorCodec cursors;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::size_t active = 0;
  bool accepting = true;
  bool delivery_failed = false;
  std::atomic<std::size_t> fixed_dispatch_attempts{0};
};

struct FloatLayoutService::RequestData {
  enum class Kind {
    RAW_READ,
    CATALOG,
    ADMIT,
    ASSET,
    LAYOUT_READ,
    LAYOUT_COMMIT
  };
  Kind kind = Kind::RAW_READ;
  CatalogQuery query;
  std::vector<SurfaceLookup> targets;
  std::optional<protocol::AssetSelector> asset;
  drogon::HttpRequestPtr http;
  AssetCompletion asset_completion;
  std::optional<layout_contract::ParsedFloatCommit> commit;
};

struct FloatLayoutService::TargetProof {
  authority::SurfaceTarget target;
  std::shared_ptr<const packages::generation::CapturedArtifact> artifact;
  std::optional<PersistenceProjection> projection;
  std::unique_ptr<capabilities::drain::DispatchGuard> guard;
  bool chrome_essential = false;
};

struct FloatLayoutService::ReadOperation {
  // Released LAST. Native callbacks retain an alias owning this whole request,
  // even after JS stops waiting; no callback retains a raw service pointer.
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  struct SlotRelease {
    std::shared_ptr<State> state;
    bool registered = false;
    ~SlotRelease() {
      if (registered) {
        std::lock_guard lock{state->mutex};
        --state->active;
        state->changed.notify_all();
      }
    }
  };
  // Asset callbacks must never alias the Operation itself: the Operation can
  // temporarily own its response, whose source would then own it back. This
  // independent physical owner contains no request/response/completion edge.
  struct PhysicalOwner {
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
    SlotRelease slot;
    packages::generation::BudgetTicket request_charge;
    packages::generation::BudgetTicket active_charge;
  };
  std::shared_ptr<PhysicalOwner> physical;
  std::shared_ptr<State> state;
  std::unique_ptr<RequestData> request;
  Completion completion;
  std::optional<drogon::HttpResponsePtr> asset_response;
  std::stop_token stop;
  auth::AuthContext auth;
  std::string ip;
  int depth = 0;
  OwnedFrontendWorker::Deadline deadline;
  std::optional<capabilities::UserContext> caller;
  std::optional<extensions::PoolIdentity> binding;
  std::unique_ptr<capabilities::drain::DispatchGuard> guard;
  std::vector<std::unique_ptr<capabilities::drain::DispatchGuard>>
      provider_guards;
  std::unique_ptr<capabilities::drain::DispatchGuard> compatibility_guard;
  std::string platform_profile;
  std::optional<js::PromiseRejection> rejection;
  struct NativeBridge {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<Result> result;
    bool published = false;
    bool started = false;
    bool sealed = false;
    bool failed = false;
  } native;
  std::optional<Result> output;
  std::atomic<bool> delivered{false};
  // SlotRelease is destroyed AFTER response/identity/guard resources, before
  // the accepted process lease. A destructor-body decrement would permit a
  // successor request before those physical members had actually disappeared.

  auto publish_native(Result value) -> void {
    std::lock_guard lock{native.mutex};
    if (native.published) {
      std::lock_guard state_lock{state->mutex};
      state->delivery_failed = true;
      return;
    }
    native.published = true;
    // A timed-out logical consumer cannot resume metadata, retry, or answer
    // twice. Native aliases still retain this request until physical disposal.
    if (!native.sealed) {
      try {
        native.result.emplace(std::move(value));
      } catch (...) {
        native.failed = true;
      }
    }
    native.changed.notify_all();
  }

  auto fail_native() noexcept -> void {
    // Loop/coroutine failure paths must not allocate an error payload: even a
    // second bad_alloc inside their catch must not escape into AsyncTask.
    std::lock_guard lock{native.mutex};
    native.failed = true;
    native.published = true;
    native.changed.notify_all();
  }

  auto finish(Result result) -> void {
    {
      std::lock_guard lock{native.mutex};
      native.sealed = true;
    }
    if (delivered.exchange(true)) {
      std::lock_guard lock{state->mutex};
      state->delivery_failed = true;
      return;
    }
    // Drop the response/frame closure after logical delivery, rather than keep
    // it alive with a late native database callback. The physical slot remains.
    auto callback = std::move(completion);
    try {
      if (request && request->asset_completion) {
        auto asset_callback = std::move(request->asset_completion);
        // The response/source independently owns its accepted physical alias.
        // Do not retain the HTTP request once delivery is logically finished.
        request->http.reset();
        if (!result)
          asset_callback(std::unexpected(std::move(result.error())));
        else if (asset_response)
          asset_callback(std::move(*asset_response));
        else
          asset_callback(std::unexpected(unavailable()));
      } else {
        callback(std::move(result));
      }
    } catch (...) {
      std::lock_guard lock{state->mutex};
      state->delivery_failed = true;
    }
  }
};

FloatLayoutService::FloatLayoutService(
    Config config,
    std::shared_ptr<packages::generation::InstalledGenerationRegistry> registry,
    std::shared_ptr<const packages::restore::ProtectedPolicy> policy,
    std::shared_ptr<TypedAssetStreams> streams)
    : state(std::make_shared<State>(std::move(config), std::move(registry),
                                    std::move(policy), std::move(streams))),
      worker(std::make_unique<OwnedFrontendWorker>(MAX_PHYSICAL_REQUESTS)) {
}

FloatLayoutService::~FloatLayoutService() {
  static_cast<void>(
      drain(std::chrono::steady_clock::now() + std::chrono::seconds{5}));
  if (!worker->physical_joined() || active_count() != 0) std::terminate();
}

auto FloatLayoutService::read_layout_storage(
    auth::AuthContext auth, std::string trusted_ip, int caller_depth,
    Completion completion, std::shared_ptr<lifecycle::AsyncTaskLease> accepted)
    -> void {
  try {
    auto request = std::make_unique<RequestData>();
    submit(std::move(request), std::move(auth), std::move(trusted_ip),
           caller_depth, std::move(completion), std::move(accepted));
  } catch (...) {
    reject_setup(std::move(completion));
  }
}

auto FloatLayoutService::catalog(
    CatalogQuery query, auth::AuthContext auth, std::string trusted_ip,
    Completion completion, std::shared_ptr<lifecycle::AsyncTaskLease> accepted)
    -> void {
  try {
    auto request = std::make_unique<RequestData>();
    request->kind = RequestData::Kind::CATALOG;
    request->query = std::move(query);
    submit(std::move(request), std::move(auth), std::move(trusted_ip), 0,
           std::move(completion), std::move(accepted));
  } catch (...) {
    reject_setup(std::move(completion));
  }
}

auto FloatLayoutService::admit(
    std::vector<SurfaceLookup> targets, std::string trusted_origin,
    auth::AuthContext auth, std::string trusted_ip, Completion completion,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted) -> void {
  try {
    auto request = std::make_unique<RequestData>();
    request->kind = RequestData::Kind::ADMIT;
    request->targets = std::move(targets);
    request->query.trusted_origin = std::move(trusted_origin);
    submit(std::move(request), std::move(auth), std::move(trusted_ip), 0,
           std::move(completion), std::move(accepted));
  } catch (...) {
    reject_setup(std::move(completion));
  }
}

auto FloatLayoutService::serve_asset(
    protocol::AssetSelector selector, drogon::HttpRequestPtr http,
    auth::AuthContext auth, std::string trusted_ip, AssetCompletion completion,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted) -> void {
  try {
    auto request = std::make_unique<RequestData>();
    request->kind = RequestData::Kind::ASSET;
    request->asset = std::move(selector);
    request->http = std::move(http);
    request->asset_completion = std::move(completion);
    submit(std::move(request), std::move(auth), std::move(trusted_ip), 0, {},
           std::move(accepted));
  } catch (...) {
    reject_setup({}, std::move(completion));
  }
}

auto FloatLayoutService::read_layout(
    std::string trusted_origin, auth::AuthContext auth, std::string trusted_ip,
    Completion completion, std::shared_ptr<lifecycle::AsyncTaskLease> accepted)
    -> void {
  try {
    auto request = std::make_unique<RequestData>();
    request->kind = RequestData::Kind::LAYOUT_READ;
    request->query.trusted_origin = std::move(trusted_origin);
    submit(std::move(request), std::move(auth), std::move(trusted_ip), 0,
           std::move(completion), std::move(accepted));
  } catch (...) {
    reject_setup(std::move(completion));
  }
}

auto FloatLayoutService::commit_layout(
    layout_contract::ParsedFloatCommit commit, auth::AuthContext auth,
    std::string trusted_ip, Completion completion,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted) -> void {
  try {
    auto request = std::make_unique<RequestData>();
    request->kind = RequestData::Kind::LAYOUT_COMMIT;
    request->commit = std::move(commit);
    submit(std::move(request), std::move(auth), std::move(trusted_ip), 0,
           std::move(completion), std::move(accepted));
  } catch (...) {
    reject_setup(std::move(completion));
  }
}

auto FloatLayoutService::reject_setup(Completion completion,
                                      AssetCompletion asset_completion) noexcept
    -> void {
  try {
    if (asset_completion)
      asset_completion(std::unexpected(unavailable()));
    else if (completion)
      completion(std::unexpected(unavailable()));
    else
      throw std::runtime_error("missing frontend completion");
  } catch (...) {
    std::lock_guard lock{state->mutex};
    state->delivery_failed = true;
  }
}

auto FloatLayoutService::submit(
    std::unique_ptr<RequestData> request, auth::AuthContext auth,
    std::string trusted_ip, int caller_depth, Completion completion,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted) -> void {
  if (!completion && !request->asset_completion) {
    std::lock_guard lock{state->mutex};
    state->delivery_failed = true;
    return;
  }
  std::shared_ptr<ReadOperation> operation;
  try {
    if (!accepted) accepted = lifecycle::async_tasks().try_acquire();
    auto budget = state->registry->source_budget();
    auto request_charge = budget.try_reserve(REQUEST_COPY_BYTES);
    if (!request_charge) {
      auto callback = std::move(completion);
      try {
        if (request->asset_completion) {
          auto asset_callback = std::move(request->asset_completion);
          asset_callback(std::unexpected(unavailable("request_busy")));
        } else {
          callback(std::unexpected(unavailable("request_busy")));
        }
      } catch (...) {
        std::lock_guard lock{state->mutex};
        state->delivery_failed = true;
      }
      return;
    }
    operation = std::make_shared<ReadOperation>();
    operation->accepted = std::move(accepted);
    operation->physical = std::make_shared<ReadOperation::PhysicalOwner>();
    operation->physical->accepted = operation->accepted;
    operation->state = state;
    operation->request = std::move(request);
    operation->physical->slot.state = state;
    operation->physical->request_charge = std::move(*request_charge);
    operation->completion = std::move(completion);
    operation->auth = std::move(auth);
    operation->ip = std::move(trusted_ip);
    operation->depth = caller_depth;
    operation->deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{10};
    // Native callers receive the same pre-dispatch refusal as HTTP callers.
    // finish owns exactly-once completion even if that callback throws. No
    // worker, PG operation or fixed resolver dispatch has been started here.
    if (operation->request->kind == RequestData::Kind::CATALOG &&
        !valid_catalog_filters(operation->request->query.filters)) {
      operation->finish(std::unexpected(unavailable("invalid_argument")));
      return;
    }
    if (!operation->accepted || caller_depth < 0 ||
        caller_depth >= capabilities::MAX_CALL_DEPTH || operation->ip.empty() ||
        operation->ip.size() > 128) {
      operation->finish(std::unexpected(unavailable("invalid_argument")));
      return;
    }
    std::string_view admission_error;
    {
      std::lock_guard lock{state->mutex};
      if (!state->accepting)
        admission_error = "cap.cancelled";
      else if (state->active >= MAX_PHYSICAL_REQUESTS)
        admission_error = "request_busy";
      else {
        ++state->active;
        operation->physical->slot.registered = true;
      }
    }
    if (!admission_error.empty()) {
      operation->finish(std::unexpected(unavailable(admission_error)));
      return;
    }
    const auto submitted = worker->try_submit(
        [operation](std::stop_token stop, auto deadline) {
          operation->stop = stop;
          auto budget = operation->state->registry->source_budget();
          auto charge = budget.try_reserve(ACTIVE_COPY_BYTES);
          if (!charge) {
            operation->rejection = unavailable("request_busy");
            return;
          }
          operation->physical->active_charge = std::move(*charge);
          const auto platform = shell::current_platform();
          if (!platform) {
            operation->rejection = unavailable();
            return;
          }
          operation->platform_profile = platform->profile();
          db::OperationScope scope{stop, deadline};
          const std::shared_ptr<PGconn> connection(
              db::connect(
                  db::connection_info(operation->state->config.db).c_str()),
              PQfinish);
          if (!connection || PQstatus(connection.get()) != CONNECTION_OK) {
            operation->rejection = unavailable();
            return;
          }
          const bool admin_candidate =
              operation->request->kind == RequestData::Kind::CATALOG &&
              operation->request->query.filters.include_admin_default;
          if (operation->request->kind != RequestData::Kind::RAW_READ &&
              operation->request->kind != RequestData::Kind::LAYOUT_READ &&
              operation->request->kind != RequestData::Kind::LAYOUT_COMMIT &&
              !admin_candidate) {
            metadata(operation, connection);
            return;
          }
          if (operation->request->kind == RequestData::Kind::LAYOUT_COMMIT) {
            if (!prepare_commit(operation, connection)) return;
          } else {
            if (admin_candidate &&
                (!operation->request->query.filters.content_type ||
                 !is_canonical_content_type(
                     *operation->request->query.filters.content_type))) {
              operation->rejection = unavailable("invalid_argument");
              return;
            }
            // Same nonblocking installer name protocol, before any shell
            // DispatchGuard. Never wait for a lifecycle lock while holding the
            // guard which that lifecycle operation must drain.
            auto name = packages::name_operation::NameOperation::acquire(
                connection, "shell");
            if (!name) {
              operation->rejection = unavailable();
              return;
            }
            auto caller =
                admin_candidate
                    ? authority::authenticate(*connection, operation->auth)
                    : authority::require_shell_preference_rule(
                          *connection, operation->auth, false);
            if (!caller) {
              operation->rejection = authority_error(caller.error());
              return;
            }
            const auto platform = shell::current_platform();
            const auto storage = extensions::fixed_shell_storage_binding();
            if (!platform || !storage ||
                storage->identity.package_id != platform->key().package_id ||
                storage->identity.name != "shell" ||
                storage->identity.version != platform->key().version ||
                storage->artifact_digest != platform->artifact_digest() ||
                capabilities::drain::is_fenced("shell") ||
                capabilities::drain::is_application_blocked("shell") ||
                !current_frontend_matches(*connection, *platform)) {
              operation->rejection = unavailable();
              return;
            }
            const auto captured = operation->state->registry->current_locked(
                *name, platform->key(), deadline, stop);
            if (!captured ||
                (*captured)->digest() != platform->artifact_digest()) {
              operation->rejection = unavailable();
              return;
            }
            scope.checkpoint();
            auto guard =
                std::make_unique<capabilities::drain::DispatchGuard>("shell");
            if (!guard->admitted() ||
                capabilities::drain::is_application_blocked("shell")) {
              operation->rejection = unavailable();
              return;
            }
            // Linearize admission while the same name lock still protects the
            // proof. This SAME guard crosses queue/frame/native callbacks; the
            // next lifecycle operation must drain it before cutover.
            operation->guard = std::move(guard);
            caller->ip_address = operation->ip;
            operation->caller = std::move(*caller);
            operation->binding = storage->identity;
            // The PG/name owner remains thread-confined. Release the name lock
            // AFTER counted admission, BEFORE dispatching native asynchronous
            // work; keep only the idle metadata connection on this worker.
            if (!name->release_checked()) {
              operation->rejection = unavailable();
              return;
            }
          }
          drogon::app().getLoop()->queueInLoop(
              [operation] { invoke_read(operation); });
          std::unique_lock lock{operation->native.mutex};
          const bool arrived =
              operation->native.changed.wait_until(lock, stop, deadline, [&] {
                return operation->native.published;
              });
          operation->native.sealed = true;
          if (!arrived || operation->native.failed ||
              !operation->native.result) {
            operation->rejection = unavailable();
            return;
          }
          operation->output = std::move(operation->native.result);
          lock.unlock();
          if (admin_candidate) {
            if (!operation->output || !*operation->output) return;
            metadata(operation, connection);
          } else if (operation->request->kind ==
                     RequestData::Kind::LAYOUT_READ) {
            if (!operation->output || !*operation->output) return;
            restore_layout(operation, connection);
          } else if (operation->request->kind ==
                         RequestData::Kind::LAYOUT_COMMIT &&
                     operation->output && *operation->output) {
            const auto& result = **operation->output;
            if (!result.isObject() || result.size() != 1 ||
                !result["saved"].isBool() || !result["saved"].asBool()) {
              operation->rejection = unavailable();
              return;
            }
            Json::Value saved{Json::objectValue};
            saved["schema_version"] = 1;
            saved["saved"] = true;
            operation->output = std::move(saved);
          }
          // Future guarded layout derivation continues HERE in this SAME
          // Work/PG thread/deadline. Never recursively submit from completion
          // while this worker still owns its physical job and closures.
        },
        [operation](FrontendWorkResult result) {
          if (!result || operation->rejection) {
            operation->finish(
                std::unexpected(operation->rejection.value_or(unavailable())));
            return;
          }
          if (!operation->output)
            operation->finish(std::unexpected(unavailable()));
          else
            operation->finish(std::move(*operation->output));
        },
        operation->accepted, operation->deadline);
    if (!submitted)
      operation->finish(std::unexpected(unavailable(
          submitted.error() == FrontendSubmitError::BUSY ? "request_busy"
                                                         : "cap.cancelled")));
  } catch (...) {
    // Setup/closure/frame allocation failure must not silently discard the
    // owned completion. If even redacted failure delivery cannot allocate,
    // keep that failure sticky for the shutdown owner rather than escaping.
    try {
      if (operation &&
          (operation->completion ||
           (operation->request && operation->request->asset_completion)))
        operation->finish(std::unexpected(unavailable()));
      else if (request && request->asset_completion) {
        auto asset_callback = std::move(request->asset_completion);
        asset_callback(std::unexpected(unavailable()));
      } else if (completion)
        completion(std::unexpected(unavailable()));
      else {
        std::lock_guard lock{state->mutex};
        state->delivery_failed = true;
      }
    } catch (...) {
      std::lock_guard lock{state->mutex};
      state->delivery_failed = true;
    }
  }
}

auto FloatLayoutService::existing_generation(
    std::shared_ptr<ReadOperation> operation,
    packages::name_operation::NameOperation& name,
    const packages::generation::GenerationKey& key)
    -> std::expected<
        std::shared_ptr<const packages::generation::CapturedArtifact>,
        js::PromiseRejection> {
  namespace generation = packages::generation;
  auto current = operation->state->registry->current_locked(
      name, key, operation->deadline, operation->stop);
  if (current) return std::move(*current);
  if (current.error() != generation::InstalledGenerationError::RECEIPT_REQUIRED)
    return std::unexpected(unavailable());
  // Explicit first-existing-v2 bootstrap, NOT implicit mutation in registry
  // reads. Caller already freshly proved its selected typed declaration under
  // this same name lock. A counted guard prevents lifecycle cutover while the
  // receipt transaction is acknowledged; no caller-supplied bypass exists.
  auto guard = std::make_unique<capabilities::drain::DispatchGuard>(key.name);
  if (!guard->admitted() ||
      capabilities::drain::is_application_blocked(key.name))
    return std::unexpected(unavailable());
  using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
  auto* connection = name.connection();
  const Result begin{db::exec(connection, "BEGIN"), PQclear};
  if (!begin || PQresultStatus(begin.get()) != PGRES_COMMAND_OK ||
      std::string_view{PQcmdStatus(begin.get())} != "BEGIN")
    return std::unexpected(unavailable());
  auto staged = operation->state->registry->stage_locked(
      name, key, operation->deadline, operation->stop);
  if (!staged) {
    // Before any COMMIT attempt the owned transaction is known uncommitted.
    // Do not retry, backfill guessed bytes, or turn a SQL error into absence.
    if (PQstatus(connection) == CONNECTION_OK &&
        (PQtransactionStatus(connection) == PQTRANS_INTRANS ||
         PQtransactionStatus(connection) == PQTRANS_INERROR)) {
      const Result rollback{db::exec(connection, "ROLLBACK"), PQclear};
      if (!rollback || PQresultStatus(rollback.get()) != PGRES_COMMAND_OK)
        capabilities::drain::block_application(key.name);
    }
    return std::unexpected(unavailable());
  }
  Result committed{nullptr, PQclear};
  try {
    committed.reset(db::exec(connection, "COMMIT"));
  } catch (...) {
    // The cancellation/deadline wrapper can throw after COMMIT was sent.
    // Preserve the unknown-outcome fence before releasing name ownership.
    capabilities::drain::block_application(key.name);
    throw;
  }
  if (!committed || PQresultStatus(committed.get()) != PGRES_COMMAND_OK ||
      std::string_view{PQcmdStatus(committed.get())} != "COMMIT") {
    capabilities::drain::block_application(key.name);
    return std::unexpected(unavailable());
  }
  auto published = operation->state->registry->publish_committed_locked(
      name, *staged, operation->deadline, operation->stop);
  if (!published) return std::unexpected(unavailable());
  return staged->artifact();
}

auto FloatLayoutService::prove_target(
    std::shared_ptr<ReadOperation> operation,
    std::shared_ptr<PGconn> connection, const SurfaceLookup& lookup,
    packages::name_operation::NameOperation* held)
    -> std::expected<std::optional<TargetProof>, js::PromiseRejection> {
  std::optional<packages::name_operation::NameOperation> owned;
  if (!held) {
    auto acquired = packages::name_operation::NameOperation::acquire(
        connection, lookup.application_id);
    if (!acquired) return std::unexpected(unavailable());
    owned = std::move(*acquired);
    held = &*owned;
  }
  if (held->name() != lookup.application_id ||
      held->connection() != connection.get() || !held->assert_owned())
    return std::unexpected(unavailable());
  auto found = authority::lookup_surface(*connection, operation->auth,
                                         lookup.application_id, lookup.panel_id,
                                         lookup.kind);
  if (!found) return std::unexpected(authority_error(found.error()));
  if (!found->target) return std::nullopt;
  auto target = std::move(*found->target);
  const auto& panel = target.declaration;
  if (kind_name(lookup.kind).empty() ||
      (lookup.kind == SurfaceKind::FLOAT &&
       (!panel.capability || !panel.context_schema)))
    return std::unexpected(unavailable());
  auto artifact = existing_generation(
      operation, *held,
      {target.generation, target.application_id, target.version, 1});
  if (!artifact) return std::unexpected(unavailable());
  const auto identity = extensions::pool_identity(target.application_id);
  const auto* entry = (*artifact)->find("client/panels/" + panel.client_path);
  const char* parameters[] = {target.generation.c_str()};
  const std::unique_ptr<PGresult, decltype(&PQclear)> declared_entry{
      db::exec_params(connection.get(),
                      "SELECT CASE WHEN octet_length(entry_point)<=1024 THEN "
                      "entry_point END "
                      "FROM plinth.packages WHERE id=$1::uuid",
                      1, nullptr, parameters, nullptr, nullptr, 0),
      PQclear};
  if (!declared_entry ||
      PQresultStatus(declared_entry.get()) != PGRES_TUPLES_OK ||
      PQntuples(declared_entry.get()) != 1 ||
      PQgetisnull(declared_entry.get(), 0, 0))
    return std::unexpected(unavailable());
  const auto* runtime_entry =
      (*artifact)->find(PQgetvalue(declared_entry.get(), 0, 0));
  if (!identity || identity->package_id != target.generation ||
      identity->version != target.version ||
      identity->name != target.application_id || !entry || !runtime_entry ||
      runtime_entry->kind != packages::restore::ArtifactKind::REGULAR ||
      entry->kind != packages::restore::ArtifactKind::REGULAR ||
      !packages::asset_server::has_registered_route(target.application_id,
                                                    target.version) ||
      capabilities::drain::is_fenced(target.application_id) ||
      capabilities::drain::is_application_blocked(target.application_id))
    return std::unexpected(unavailable());
  std::optional<PersistenceProjection> projection;
  if (lookup.kind == SurfaceKind::FLOAT) {
    const auto parsed = capabilities::parse_signature(*panel.capability);
    const auto* signature = std::get_if<capabilities::ParsedSignature>(&parsed);
    const auto* handler = signature
                              ? (*artifact)->find("server/handlers/" +
                                                  signature->function + ".js")
                              : nullptr;
    if (!signature || signature->namespace_ != target.application_id ||
        !handler || handler->kind != packages::restore::ArtifactKind::REGULAR)
      return std::unexpected(unavailable());
  }
  if (lookup.kind == SurfaceKind::FLOAT && operation->state->policy) {
    auto reviewer = authority::reviewer_is_admin(
        *connection, operation->state->policy->parsed().review.admin_user_id);
    if (!reviewer) return std::unexpected(authority_error(reviewer.error()));
    if (*reviewer)
      projection = match_persistence_projection(
          *operation->state->policy, target.application_id, panel.id,
          *panel.capability, (*artifact)->digest(), *panel.context_schema);
  }
  auto guard = std::make_unique<capabilities::drain::DispatchGuard>(
      target.application_id);
  if (!guard->admitted() ||
      capabilities::drain::is_application_blocked(target.application_id))
    return std::unexpected(unavailable());
  // Essential is presentation metadata, never permission. The platform guard
  // was established by this same request and stays counted through delivery.
  // Foreign declarations (including their raw flag) cannot become essential.
  const auto platform = shell::current_platform();
  const bool essential =
      lookup.kind == SurfaceKind::TRAY && operation->compatibility_guard &&
      platform && platform->profile() == operation->platform_profile &&
      target.application_id == "shell" &&
      target.generation == platform->key().package_id &&
      target.version == platform->key().version &&
      (*artifact)->digest() == platform->artifact_digest() &&
      panel.rbac_rule == "shell.preferences.read" &&
      ((panel.id == "bell" && panel.client_path == "shell-bell.js") ||
       (panel.id == "avatar" && panel.client_path == "shell-avatar.js"));
  if (owned && !owned->release_checked()) return std::unexpected(unavailable());
  return TargetProof{std::move(target), std::move(*artifact),
                     std::move(projection), std::move(guard), essential};
}

auto FloatLayoutService::prepare_commit(
    std::shared_ptr<ReadOperation> operation,
    std::shared_ptr<PGconn> connection) -> bool {
  using Wire = nlohmann::json;
  const auto reject = [&](std::string_view code = "storage_unavailable") {
    operation->rejection = unavailable(code);
    return false;
  };
  if (!operation->request->commit) return reject("invalid_argument");
  const auto& commit = *operation->request->commit;
  const auto& layout = commit.layout;
  if (!layout.is_object() || layout.size() != 2 ||
      !layout.contains("version") || layout["version"] != 1 ||
      !layout.contains("floats") || !layout["floats"].is_array() ||
      layout["floats"].size() > 5 ||
      commit.bindings.size() != layout["floats"].size() ||
      layout.dump().size() > 32768)
    return reject("invalid_argument");
  if (!operation->state->policy) return reject();
  auto reviewer = authority::reviewer_is_admin(
      *connection, operation->state->policy->parsed().review.admin_user_id);
  if (!reviewer) {
    operation->rejection = authority_error(reviewer.error());
    return false;
  }
  if (!*reviewer) return reject();
  std::set<std::string> names{"shell"};
  std::set<std::string> identities;
  for (const auto& entry : layout["floats"]) {
    auto identity = layout_contract::saved_float_identity(entry);
    if (!identity)
      return reject(identity.error() == layout_contract::Error::LIMIT_EXCEEDED
                        ? "storage_unavailable"
                        : "invalid_argument");
    if (!identities.insert(*identity).second) return reject("invalid_argument");
    names.insert(entry["application_id"].get<std::string>());
  }
  // All unique names, in stable order, BEFORE taking any counted guard. Each
  // acquisition is nonwaiting; no guard/name inversion with lifecycle drains.
  std::vector<packages::name_operation::NameOperation> locks;
  locks.reserve(names.size());
  for (const auto& name : names) {
    auto acquired =
        packages::name_operation::NameOperation::acquire(connection, name);
    if (!acquired) return reject();
    locks.push_back(std::move(*acquired));
  }
  auto caller = authority::require_shell_preference_rule(*connection,
                                                         operation->auth, true);
  if (!caller) {
    operation->rejection = authority_error(caller.error());
    return false;
  }
  const auto platform = shell::current_platform();
  const auto storage = extensions::fixed_shell_storage_binding();
  if (!platform || !storage || storage->identity.name != "shell" ||
      storage->identity.package_id != platform->key().package_id ||
      storage->identity.version != platform->key().version ||
      storage->artifact_digest != platform->artifact_digest() ||
      !current_frontend_matches(*connection, *platform))
    return reject();
  auto shell_lock = std::ranges::find_if(
      locks, [](const auto& lock) { return lock.name() == "shell"; });
  if (shell_lock == locks.end()) return reject();
  auto captured = operation->state->registry->current_locked(
      *shell_lock, platform->key(), operation->deadline, operation->stop);
  if (!captured || (*captured)->digest() != platform->artifact_digest())
    return reject();
  operation->provider_guards.reserve(commit.bindings.size());
  for (std::size_t index = 0; index < commit.bindings.size(); ++index) {
    const auto& entry = layout["floats"][index];
    const auto& binding = commit.bindings[index];
    if (binding.index != index) return reject("invalid_argument");
    SurfaceLookup lookup{.application_id =
                             entry["application_id"].get<std::string>(),
                         .panel_id = entry["panel_id"].get<std::string>(),
                         .kind = SurfaceKind::FLOAT,
                         .expected_generation = binding.generation,
                         .content_type = std::nullopt,
                         .capability = entry["capability"].get<std::string>()};
    auto held = std::ranges::find_if(locks, [&](const auto& lock) {
      return lock.name() == lookup.application_id;
    });
    if (held == locks.end()) return reject();
    auto proof = prove_target(operation, connection, lookup, &*held);
    if (!proof) {
      operation->rejection = std::move(proof.error());
      return false;
    }
    if (!*proof || !(**proof).projection) return reject("stale_binding");
    const auto& target = (**proof).target;
    const auto& projection = *(**proof).projection;
    if (target.generation != binding.generation ||
        target.declaration.capability != lookup.capability ||
        projection.artifact_digest != binding.artifact_digest ||
        projection.schema_digest != binding.schema_digest ||
        projection.projection_contract.id != binding.projection_contract.id ||
        projection.projection_contract.version !=
            binding.projection_contract.version ||
        projection.policy_revision != binding.policy_revision)
      return reject("stale_binding");
    auto projected = project_float_context(
        projection, *target.declaration.context_schema, entry["context"]);
    if (!projected || *projected != entry["context"])
      return reject("stale_binding");
    operation->provider_guards.push_back(std::move((**proof).guard));
  }
  auto guard = std::make_unique<capabilities::drain::DispatchGuard>("shell");
  if (!guard->admitted() ||
      capabilities::drain::is_application_blocked("shell"))
    return reject();
  operation->guard = std::move(guard);
  caller->ip_address = operation->ip;
  operation->caller = std::move(*caller);
  operation->binding = storage->identity;
  for (auto& lock : locks)
    if (!lock.release_checked()) return reject();
  return true;
}

auto FloatLayoutService::restore_layout(
    std::shared_ptr<ReadOperation> operation,
    std::shared_ptr<PGconn> connection) -> void {
  using Wire = nlohmann::json;
  if (!guard_platform(operation, connection)) return;
  const auto& native = **operation->output;
  if (!native.isObject() || !native["present"].isBool() ||
      native.size() != (native["present"].asBool() ? 2 : 1) ||
      (native["present"].asBool() && !native.isMember("value"))) {
    operation->rejection = unavailable();
    return;
  }
  Wire revision = nullptr;
  if (operation->state->policy) {
    auto reviewer = authority::reviewer_is_admin(
        *connection, operation->state->policy->parsed().review.admin_user_id);
    if (!reviewer) {
      operation->rejection = authority_error(reviewer.error());
      return;
    }
    if (*reviewer)
      revision = operation->state->policy->parsed().policy_revision;
  }
  Wire output{{"schema_version", 1},
              {"present", native["present"].asBool()},
              {"policy_revision", revision},
              {"restore_targets", Wire::array()}};
  if (native["present"].asBool()) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    const auto text = Json::writeString(writer, native["value"]);
    if (text.size() > 32768) {
      operation->rejection = unavailable();
      return;
    }
    auto raw = Wire::parse(text, nullptr, false);
    if (raw.is_discarded()) {
      operation->rejection = unavailable();
      return;
    }
    output["layout"] = raw; // Return original value; never repair/write it.
    if (raw.is_object() && raw.size() == 2 && raw.contains("version") &&
        raw["version"] == 1 && raw.contains("floats") &&
        raw["floats"].is_array() && raw["floats"].size() <= 5) {
      std::set<std::string> seen;
      for (std::size_t index = 0; index < raw["floats"].size(); ++index) {
        const auto& original = raw["floats"][index];
        auto entry = layout_contract::normalize_saved_float_entry(original);
        auto identity = layout_contract::saved_float_identity(original);
        if ((!entry &&
             entry.error() == layout_contract::Error::LIMIT_EXCEEDED) ||
            (!identity &&
             identity.error() == layout_contract::Error::LIMIT_EXCEEDED)) {
          operation->rejection = unavailable();
          return;
        }
        Wire result{{"index", index}, {"status", "unavailable"}};
        if (entry && identity && seen.insert(*identity).second &&
            !revision.is_null()) {
          SurfaceLookup lookup{
              .application_id = (*entry)["application_id"].get<std::string>(),
              .panel_id = (*entry)["panel_id"].get<std::string>(),
              .kind = SurfaceKind::FLOAT,
              .expected_generation = std::nullopt,
              .content_type = std::nullopt,
              .capability = (*entry)["capability"].get<std::string>()};
          auto proof = prove_target(operation, connection, lookup);
          if (!proof) {
            operation->rejection = std::move(proof.error());
            return;
          }
          if (*proof && (**proof).projection &&
              (**proof).target.declaration.capability == lookup.capability) {
            const auto& projection = *(**proof).projection;
            auto projected = project_float_context(
                projection, *(**proof).target.declaration.context_schema,
                (*entry)["context"]);
            if (projected && *projected == (*entry)["context"]) {
              auto encoded = protocol::encode_float_target(
                  (**proof).target, *(**proof).artifact, projection,
                  operation->request->query.trusted_origin);
              if (!encoded) {
                operation->rejection = unavailable();
                return;
              }
              result["status"] = "available";
              result["target"] = std::move(*encoded);
            }
          }
        }
        output["restore_targets"].push_back(std::move(result));
      }
    }
  }
  if (output.dump().size() > std::size_t{128} * 1024) {
    operation->rejection = unavailable();
    return;
  }
  operation->output = jsoncpp(output);
}

auto FloatLayoutService::guard_platform(
    std::shared_ptr<ReadOperation> operation,
    std::shared_ptr<PGconn> connection) -> bool {
  auto name =
      packages::name_operation::NameOperation::acquire(connection, "shell");
  const auto platform = shell::current_platform();
  const auto storage = extensions::fixed_shell_storage_binding();
  if (!name || !platform || !storage ||
      platform->profile() != operation->platform_profile ||
      storage->identity.package_id != platform->key().package_id ||
      storage->identity.name != "shell" ||
      storage->identity.version != platform->key().version ||
      storage->artifact_digest != platform->artifact_digest() ||
      !current_frontend_matches(*connection, *platform)) {
    operation->rejection = unavailable();
    return false;
  }
  const auto artifact = operation->state->registry->current_locked(
      *name, platform->key(), operation->deadline, operation->stop);
  if (!artifact || (*artifact)->digest() != platform->artifact_digest()) {
    operation->rejection = unavailable();
    return false;
  }
  auto guard = std::make_unique<capabilities::drain::DispatchGuard>("shell");
  if (!guard->admitted() ||
      capabilities::drain::is_application_blocked("shell") ||
      !name->release_checked()) {
    operation->rejection = unavailable();
    return false;
  }
  operation->compatibility_guard = std::move(guard);
  return true;
}

auto FloatLayoutService::metadata(std::shared_ptr<ReadOperation> operation,
                                  std::shared_ptr<PGconn> connection) -> void {
  using Wire = nlohmann::json;
  auto& request = *operation->request;
  const auto platform = shell::current_platform();
  if (!platform) {
    operation->rejection = unavailable();
    return;
  }
  if (request.kind == RequestData::Kind::CATALOG ||
      request.kind == RequestData::Kind::ADMIT) {
    const auto caller = authority::authenticate(*connection, operation->auth);
    if (!caller) {
      operation->rejection = authority_error(caller.error());
      return;
    }
    if (!guard_platform(operation, connection)) return;
  }
  auto target_wire = [&](const TargetProof& proof) {
    switch (proof.target.kind) {
      case SurfaceKind::PRIMARY:
        return protocol::encode_primary_target(proof.target, *proof.artifact,
                                               request.query.trusted_origin);
      case SurfaceKind::TRAY:
        return protocol::encode_tray_target(proof.target, *proof.artifact,
                                            proof.chrome_essential,
                                            request.query.trusted_origin);
      case SurfaceKind::FLOAT:
        return protocol::encode_float_target(proof.target, *proof.artifact,
                                             proof.projection,
                                             request.query.trusted_origin);
    }
    return std::expected<Wire, protocol::Error>{
        std::unexpected(protocol::Error::INVALID)};
  };
  if (request.kind == RequestData::Kind::ADMIT) {
    if (request.targets.size() > 38 ||
        std::ranges::any_of(request.targets, [](const auto& lookup) {
          return kind_name(lookup.kind).empty() ||
                 (lookup.kind != SurfaceKind::FLOAT &&
                  (lookup.content_type || lookup.capability));
        })) {
      operation->rejection = unavailable("invalid_argument");
      return;
    }
    // Even an empty batch must revalidate the actual credential.
    auto caller = authority::authenticate(*connection, operation->auth);
    if (!caller) {
      operation->rejection = authority_error(caller.error());
      return;
    }
    Wire results = Wire::array();
    for (std::size_t index = 0; index < request.targets.size(); ++index) {
      const auto& lookup = request.targets[index];
      auto proof = prove_target(operation, connection, lookup);
      if (!proof) {
        operation->rejection = std::move(proof.error());
        return;
      }
      Wire result{{"index", index}, {"status", "unavailable"}};
      if (*proof) {
        const auto& target = (**proof).target;
        if (lookup.expected_generation &&
            (*lookup.expected_generation != target.generation ||
             (lookup.kind == SurfaceKind::FLOAT &&
              lookup.capability != target.declaration.capability))) {
          result["status"] = "stale";
        } else if (!lookup.content_type ||
                   std::ranges::find(target.declaration.content_types,
                                     *lookup.content_type) !=
                       target.declaration.content_types.end()) {
          auto encoded = target_wire(**proof);
          if (!encoded) {
            operation->rejection = unavailable();
            return;
          }
          result["status"] = "available";
          result["target"] = std::move(*encoded);
        }
      }
      results.push_back(std::move(result));
    }
    operation->output =
        jsoncpp(Wire{{"schema_version", 1}, {"results", std::move(results)}});
    return;
  }
  if (request.kind == RequestData::Kind::CATALOG) {
    auto& filters = request.query.filters;
    if (!valid_catalog_filters(filters)) {
      operation->rejection = unavailable("invalid_argument");
      return;
    }
    filters.platform_profile = platform->profile();
    std::optional<authority::Position> after;
    if (request.query.cursor) {
      auto decoded = operation->state->cursors.decode(
          *request.query.cursor, operation->auth, filters, unix_seconds());
      if (!decoded) {
        operation->rejection = unavailable("invalid_argument");
        return;
      }
      after = std::move(*decoded);
    }
    auto page =
        filters.kind != SurfaceKind::FLOAT
            ? authority::catalog_surface_prefix(*connection, operation->auth,
                                                filters.kind, filters.limit,
                                                after)
        : filters.content_type
            ? authority::catalog_float_prefix(*connection, operation->auth,
                                              *filters.content_type,
                                              filters.limit, after)
            : authority::catalog_float_prefix(*connection, operation->auth,
                                              std::nullopt, filters.limit,
                                              after);
    if (!page) {
      operation->rejection = authority_error(page.error());
      return;
    }
    Wire targets = Wire::array();
    std::size_t bytes = 8192; // envelope/cursor/admin candidate headroom
    auto next_position = page->next_position;
    std::optional<authority::Position> last;
    for (const auto& target : page->targets) {
      SurfaceLookup lookup{.application_id = target.application_id,
                           .panel_id = target.declaration.id,
                           .kind = filters.kind,
                           .expected_generation = target.generation,
                           .content_type = filters.content_type,
                           .capability = target.declaration.capability};
      auto proof = prove_target(operation, connection, lookup);
      if (!proof || !*proof ||
          (**proof).target.generation != target.generation ||
          (**proof).target.logical_install_order !=
              target.logical_install_order) {
        operation->rejection =
            !proof ? std::move(proof.error()) : unavailable();
        return;
      }
      auto encoded = target_wire(**proof);
      if (!encoded) {
        operation->rejection = unavailable();
        return;
      }
      const auto size = encoded->dump().size() + 1;
      if (bytes + size > std::size_t{1024} * 1024) {
        next_position = last;
        break;
      }
      bytes += size;
      last = authority::Position{target.logical_install_order,
                                 target.application_id, target.declaration.id};
      targets.push_back(std::move(*encoded));
    }
    Wire next = nullptr;
    if (next_position) {
      auto cursor = operation->state->cursors.encode(
          operation->auth, filters, *next_position, unix_seconds());
      if (!cursor) {
        operation->rejection = unavailable();
        return;
      }
      next = std::move(*cursor);
    }
    Wire output{{"schema_version", 1},
                {"kind", kind_name(filters.kind)},
                {"targets", std::move(targets)},
                {"next_cursor", next}};
    if (filters.include_admin_default) {
      // Filled from the real fixed storage result, never kernel SQL/default
      // table fallback. Unknown native storage must fail the whole request.
      if (!operation->output || !*operation->output) {
        operation->rejection = unavailable();
        return;
      }
      const auto& native = **operation->output;
      if (!native.isObject() || native.size() != 1 ||
          !native.isMember("candidate")) {
        operation->rejection = unavailable();
        return;
      }
      output["admin_default_candidate"] = nullptr;
      const auto& candidate = native["candidate"];
      if (!candidate.isNull()) {
        if (!candidate.isObject() || candidate.size() != 2 ||
            !candidate["application_id"].isString() ||
            !candidate["panel_id"].isString()) {
          operation->rejection = unavailable();
          return;
        }
        SurfaceLookup lookup{.application_id =
                                 candidate["application_id"].asString(),
                             .panel_id = candidate["panel_id"].asString(),
                             .kind = SurfaceKind::FLOAT,
                             .expected_generation = std::nullopt,
                             .content_type = filters.content_type,
                             .capability = std::nullopt};
        auto proof = prove_target(operation, connection, lookup);
        if (!proof) {
          operation->rejection = std::move(proof.error());
          return;
        }
        if (*proof &&
            std::ranges::find((**proof).target.declaration.content_types,
                              *filters.content_type) !=
                (**proof).target.declaration.content_types.end()) {
          output["admin_default_candidate"] = {
              {"application_id", lookup.application_id},
              {"panel_id", lookup.panel_id},
              {"generation", (**proof).target.generation}};
        }
      }
    }
    operation->output = jsoncpp(output);
    return;
  }
  if (request.kind == RequestData::Kind::ASSET && request.asset &&
      request.http) {
    const auto& selector = *request.asset;
    std::shared_ptr<const packages::generation::CapturedArtifact> artifact;
    std::unique_ptr<capabilities::drain::DispatchGuard> guard;
    if (selector.kind == protocol::AssetKind::PLATFORM) {
      if (std::ranges::none_of(shell::platform_profile::CLIENT_FILES,
                               [&](const auto& file) {
                                 return file.path == selector.member_path;
                               })) {
        operation->rejection = unavailable("asset_not_found");
        return;
      }
      auto name =
          packages::name_operation::NameOperation::acquire(connection, "shell");
      const auto storage = extensions::fixed_shell_storage_binding();
      if (!name || !storage ||
          platform->key().package_id != selector.generation ||
          platform->artifact_digest() != selector.artifact_digest ||
          storage->identity.package_id != platform->key().package_id ||
          storage->identity.name != "shell" ||
          storage->identity.version != platform->key().version ||
          storage->artifact_digest != platform->artifact_digest() ||
          !current_frontend_matches(*connection, *platform)) {
        operation->rejection = unavailable();
        return;
      }
      auto current = operation->state->registry->current_locked(
          *name, platform->key(), operation->deadline, operation->stop);
      if (!current || (*current)->digest() != selector.artifact_digest) {
        operation->rejection = unavailable();
        return;
      }
      artifact = std::move(*current);
      guard = std::make_unique<capabilities::drain::DispatchGuard>("shell");
      if (!guard->admitted() ||
          capabilities::drain::is_application_blocked("shell") ||
          !name->release_checked()) {
        operation->rejection = unavailable();
        return;
      }
    } else {
      auto caller = authority::authenticate(*connection, operation->auth);
      if (!caller) {
        operation->rejection = authority_error(caller.error());
        return;
      }
      const char* parameters[] = {selector.generation.c_str(),
                                  selector.panel_id ? selector.panel_id->c_str()
                                                    : ""};
      const std::unique_ptr<PGresult, decltype(&PQclear)> row{
          db::exec_params(connection.get(),
                          "SELECT p.name,pn.panel_type FROM plinth.packages p "
                          "JOIN plinth.panels pn ON pn.package_id=p.id "
                          "WHERE p.id=$1::uuid AND pn.panel_id=$2 "
                          "AND pn.validated_format_version=2 "
                          "AND octet_length(p.name)<=64 LIMIT 2",
                          2, nullptr, parameters, nullptr, nullptr, 0),
          PQclear};
      if (!row || PQresultStatus(row.get()) != PGRES_TUPLES_OK) {
        operation->rejection = unavailable();
        return;
      }
      if (PQntuples(row.get()) != 1 || PQnfields(row.get()) != 2 ||
          PQgetisnull(row.get(), 0, 0) || PQgetisnull(row.get(), 0, 1) ||
          !selector.panel_id) {
        operation->rejection = unavailable("asset_not_found");
        return;
      }
      const std::string_view stored_kind{PQgetvalue(row.get(), 0, 1)};
      std::optional<SurfaceKind> kind;
      if (stored_kind == "primary")
        kind = SurfaceKind::PRIMARY;
      else if (stored_kind == "tray")
        kind = SurfaceKind::TRAY;
      else if (stored_kind == "float")
        kind = SurfaceKind::FLOAT;
      if (!kind) {
        operation->rejection = unavailable("asset_not_found");
        return;
      }
      SurfaceLookup lookup{.application_id = PQgetvalue(row.get(), 0, 0),
                           .panel_id = *selector.panel_id,
                           .kind = *kind,
                           .expected_generation = selector.generation,
                           .content_type = std::nullopt,
                           .capability = std::nullopt};
      auto proof = prove_target(operation, connection, lookup);
      if (!proof) {
        operation->rejection = std::move(proof.error());
        return;
      }
      if (!*proof || (**proof).target.generation != selector.generation ||
          (**proof).artifact->digest() != selector.artifact_digest) {
        operation->rejection = unavailable("asset_not_found");
        return;
      }
      artifact = std::move((**proof).artifact);
      guard = std::move((**proof).guard);
    }
    const auto* member = artifact->find(selector.member_path);
    if (!member || member->kind != packages::restore::ArtifactKind::REGULAR ||
        !selector.member_path.starts_with("client/")) {
      operation->rejection = unavailable("asset_not_found");
      return;
    }
    const std::shared_ptr<lifecycle::AsyncTaskLease> physical{
        operation->physical, operation->accepted.get()};
    auto response = operation->state->streams->make_response(
        request.http, AdmittedAsset{std::move(artifact), selector.member_path,
                                    std::move(guard), physical});
    if (!response) {
      operation->rejection = unavailable(
          response.error() == AssetStreamError::RANGE ? "range_not_satisfiable"
                                                      : "request_busy");
      return;
    }
    operation->asset_response = std::move(*response);
    operation->output = Json::Value{Json::objectValue};
    // Parsing/serialization scratch is no longer live after this handoff.
    // Physical stream bytes, request charge and slot remain independently held.
    operation->physical->active_charge = {};
    return;
  }
  operation->rejection = unavailable("invalid_argument");
}

auto FloatLayoutService::invoke_read(std::shared_ptr<ReadOperation> operation)
    -> void {
  {
    std::lock_guard lock{operation->native.mutex};
    // Stop before loop admission must not start new SQL afterward. A native
    // call already claimed here may still finish for its original owner; its
    // physical alias remains retained even if the worker subsequently stops.
    if (operation->native.sealed) return;
    if (operation->native.started) {
      std::lock_guard state_lock{operation->state->mutex};
      operation->state->delivery_failed = true;
      return;
    }
    operation->native.started = true;
  }
  if (!operation->caller || !operation->binding ||
      std::chrono::steady_clock::now() >= operation->deadline) {
    operation->fail_native();
    return;
  }
  try {
    // Allocate the child frame BEFORE async_run enters its AsyncTask. The
    // factory below is non-coroutine/noexcept, so allocation failure cannot
    // escape from inside the framework's fatal unhandled-exception boundary.
    auto task = run_read(operation);
    drogon::async_run([task = std::move(task)]() mutable noexcept {
      return std::move(task);
    });
  } catch (...) {
    operation->fail_native();
  }
}

auto FloatLayoutService::run_read(std::shared_ptr<ReadOperation> operation)
    -> drogon::Task<> {
  try {
    const std::shared_ptr<lifecycle::AsyncTaskLease> physical(
        operation, operation->accepted.get());
    if (operation->request->kind == RequestData::Kind::LAYOUT_COMMIT) {
      operation->state->fixed_dispatch_attempts.fetch_add(1);
      auto result = co_await extensions::dispatch_fixed_shell_commit(
          extensions::ValidatedFloatCommit{
              *operation->binding, jsoncpp(operation->request->commit->layout),
              std::move(operation->guard)},
          std::move(*operation->caller), operation->depth, physical);
      operation->publish_native(std::move(result));
      co_return;
    }
    const bool admin_candidate =
        operation->request->kind == RequestData::Kind::CATALOG;
    Json::Value arguments{Json::objectValue};
    if (admin_candidate)
      arguments["content_type"] =
          *operation->request->query.filters.content_type;
    operation->state->fixed_dispatch_attempts.fetch_add(1);
    auto result = co_await extensions::dispatch_fixed_shell_storage(
        extensions::FixedStorageCall{
            admin_candidate
                ? extensions::FixedStorageOperation::RESOLVE_ADMIN_CANDIDATE
                : extensions::FixedStorageOperation::FLOATS_READ,
            *operation->binding, std::move(arguments),
            std::move(operation->guard)},
        std::move(*operation->caller), operation->depth, physical);
    operation->publish_native(std::move(result));
  } catch (...) {
    operation->fail_native();
  }
}

auto FloatLayoutService::close_admission() -> void {
  {
    std::lock_guard lock{state->mutex};
    state->accepting = false;
  }
  worker->close_admission();
  state->streams->close_admission();
}

auto FloatLayoutService::drain(std::chrono::steady_clock::time_point deadline)
    -> bool {
  close_admission();
  const bool stopped = state->streams->request_stop();
  const bool joined = worker->drain(deadline);
  const bool streams_settled = state->streams->drain(deadline);
  std::unique_lock lock{state->mutex};
  const bool settled = state->changed.wait_until(
      lock, deadline, [&] { return state->active == 0; });
  return stopped && joined && streams_settled && settled &&
         !state->delivery_failed;
}

auto FloatLayoutService::active_count() const -> std::size_t {
  std::lock_guard lock{state->mutex};
  return state->active;
}

auto test_seam::fixed_storage_dispatch_attempts(
    const FloatLayoutService& service) -> std::size_t {
  return service.state->fixed_dispatch_attempts.load();
}

} // namespace plinth::frontend
