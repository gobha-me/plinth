#pragma once

#include "kernel/auth/middleware.hpp"
#include "kernel/config.hpp"
#include "kernel/frontend/float_layout_contract.hpp"
#include "kernel/frontend/surface_protocol.hpp"
#include "kernel/frontend/typed_requests.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plinth::packages::generation {
class InstalledGenerationRegistry;
}
namespace plinth::packages::restore {
class ProtectedPolicy;
}
namespace plinth::packages::name_operation {
class NameOperation;
}

namespace plinth::frontend {

class OwnedFrontendWorker;
class TypedAssetStreams;

// Native guarded storage service, not a new SDK/capability or JSON token
// factory. HTTP layout/catalog/projection integration is a separate caller of
// this seam. This class alone constructs the fixed-runtime operation tokens.
class FloatLayoutService {
 public:
  using Result = std::expected<Json::Value, js::PromiseRejection>;
  using Completion = std::function<void(Result)>;
  using AssetResult =
      std::expected<drogon::HttpResponsePtr, js::PromiseRejection>;
  using AssetCompletion = std::function<void(AssetResult)>;
  struct CatalogQuery {
    protocol::CursorQuery filters;
    std::optional<std::string> cursor;
    std::string trusted_origin;
  };

  explicit FloatLayoutService(
      Config config,
      std::shared_ptr<packages::generation::InstalledGenerationRegistry>
          registry = nullptr,
      std::shared_ptr<const packages::restore::ProtectedPolicy> policy =
          nullptr,
      std::shared_ptr<TypedAssetStreams> streams = nullptr);
  ~FloatLayoutService();
  FloatLayoutService(const FloatLayoutService&) = delete;
  auto operator=(const FloatLayoutService&) -> FloatLayoutService& = delete;

  // Refreshes the actual credential and registered existing read gate on the
  // owned PG worker, checks authoritative bundled frontend/current fixed pool,
  // then dispatches captured storage with original trusted IP/depth. Returns
  // fixed module {present,value?}; the HTTP facade must still derive restore
  // targets and enforce its exact external envelope. No test-only bypass.
  auto read_layout_storage(
      auth::AuthContext auth, std::string trusted_ip, int caller_depth,
      Completion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr) -> void;
  auto catalog(CatalogQuery query, auth::AuthContext auth,
               std::string trusted_ip, Completion completion,
               std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr)
      -> void;
  auto admit(std::vector<SurfaceLookup> targets, std::string trusted_origin,
             auth::AuthContext auth, std::string trusted_ip,
             Completion completion,
             std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr)
      -> void;
  auto serve_asset(
      protocol::AssetSelector selector, drogon::HttpRequestPtr request,
      auth::AuthContext auth, std::string trusted_ip,
      AssetCompletion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr) -> void;
  auto read_layout(
      std::string trusted_origin, auth::AuthContext auth,
      std::string trusted_ip, Completion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr) -> void;
  auto commit_layout(
      layout_contract::ParsedFloatCommit commit, auth::AuthContext auth,
      std::string trusted_ip, Completion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr) -> void;
  auto close_admission() -> void;
  // Waits actual worker, callback and native descendant ownership, not just JS
  // completion. A failed drain must remain visible to the process coordinator.
  [[nodiscard]] auto drain(std::chrono::steady_clock::time_point deadline)
      -> bool;
  [[nodiscard]] auto active_count() const -> std::size_t;

 private:
  struct State;
  struct RequestData;
  struct ReadOperation;
  struct TargetProof;
  auto submit(std::unique_ptr<RequestData> request, auth::AuthContext auth,
              std::string trusted_ip, int caller_depth, Completion completion,
              std::shared_ptr<lifecycle::AsyncTaskLease> accepted) -> void;
  auto reject_setup(Completion completion,
                    AssetCompletion asset_completion = {}) noexcept -> void;
  static auto prove_target(
      std::shared_ptr<ReadOperation> operation,
      std::shared_ptr<PGconn> connection, const SurfaceLookup& lookup,
      packages::name_operation::NameOperation* held = nullptr)
      -> std::expected<std::optional<TargetProof>, js::PromiseRejection>;
  static auto metadata(std::shared_ptr<ReadOperation> operation,
                       std::shared_ptr<PGconn> connection) -> void;
  static auto prepare_commit(std::shared_ptr<ReadOperation> operation,
                             std::shared_ptr<PGconn> connection) -> bool;
  static auto existing_generation(
      std::shared_ptr<ReadOperation> operation,
      packages::name_operation::NameOperation& name,
      const packages::generation::GenerationKey& key)
      -> std::expected<
          std::shared_ptr<const packages::generation::CapturedArtifact>,
          js::PromiseRejection>;
  static auto restore_layout(std::shared_ptr<ReadOperation> operation,
                             std::shared_ptr<PGconn> connection) -> void;
  static auto guard_platform(std::shared_ptr<ReadOperation> operation,
                             std::shared_ptr<PGconn> connection) -> bool;
  static auto run_read(std::shared_ptr<ReadOperation> operation)
      -> drogon::Task<>;
  static auto invoke_read(std::shared_ptr<ReadOperation> operation) -> void;
  std::shared_ptr<State> state;
  std::unique_ptr<OwnedFrontendWorker> worker;
};

} // namespace plinth::frontend
