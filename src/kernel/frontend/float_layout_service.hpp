#pragma once

#include "kernel/auth/middleware.hpp"
#include "kernel/config.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/frontend/admin_defaults_contract.hpp"
#include "kernel/frontend/float_layout_contract.hpp"
#include "kernel/frontend/surface_protocol.hpp"
#include "kernel/frontend/typed_requests.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/packages/generation_cache.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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

namespace admin_ack_detail {
// Immutable identity values only. Comparison grants no authority, admission,
// reply frame or lifecycle ownership; both actual ACK fences use this same
// predicate before mapping a known mismatch to stale_binding409.
struct FrameIdentity {
  packages::generation::GenerationKey platform_key;
  std::string profile;
  std::string artifact_digest;
  extensions::PoolIdentity storage_identity;
  std::string storage_artifact_digest;
};
[[nodiscard]] auto same_identity(const FrameIdentity& admitted,
                                 const FrameIdentity& current) noexcept -> bool;
} // namespace admin_ack_detail

class OwnedFrontendWorker;
class TypedAssetStreams;
class FloatLayoutService;
namespace test_seam {
// Observation only: attempts need not have succeeded. No reset/fault/token API.
[[nodiscard]] auto fixed_storage_dispatch_attempts(const FloatLayoutService&)
    -> std::size_t;
// Per-service observation/pause only, after actual successful native settlement
// on the metadata worker. No locks/args/results/identity are exposed. Tests
// must use a finite RAII latch and clear the hook before destroying their
// fixture.
auto set_admin_native_settled_hook(FloatLayoutService&, std::function<void()>)
    -> void;
[[nodiscard]] auto admin_dispatch_caller_depth(const FloatLayoutService&)
    -> int;
} // namespace test_seam

// Native guarded storage service, not a new SDK/capability or JSON token
// factory. HTTP layout/catalog/projection integration is a separate caller of
// this seam. This class alone constructs the fixed-runtime operation tokens.
class FloatLayoutService {
  struct AdminAckOwner;

 public:
  using Result = std::expected<Json::Value, js::PromiseRejection>;
  using Completion = std::function<void(Result)>;
  using AssetResult =
      std::expected<drogon::HttpResponsePtr, js::PromiseRejection>;
  using AssetCompletion = std::function<void(AssetResult)>;
  class AdminReply {
   public:
    AdminReply(const AdminReply&) = delete;
    auto operator=(const AdminReply&) -> AdminReply& = delete;
    AdminReply(AdminReply&&) noexcept = default;
    auto operator=(AdminReply&&) noexcept -> AdminReply&;
    [[nodiscard]] auto body() const noexcept -> const Json::Value&;
    [[nodiscard]] auto profile() const noexcept -> std::string_view;
    // Call AFTER bounded serialization and immediately BEFORE sending. Uses
    // only short immutable-metadata/fence reads, never PG/name acquisition.
    // Retain this entire reply through callback return/unwind. This is a
    // server acknowledgement fence, not proof of client receipt or rollback.
    [[nodiscard]] auto prepare_handoff() const
        -> std::expected<void, js::PromiseRejection>;

   private:
    friend class FloatLayoutService;
    AdminReply(std::shared_ptr<AdminAckOwner> owner, Json::Value body);
    // Released LAST, after body resources, with no request/completion
    // back-edge.
    std::shared_ptr<AdminAckOwner> owner;
    Json::Value value;
  };
  using AdminResult = std::expected<AdminReply, js::PromiseRejection>;
  using AdminCompletion = std::function<void(AdminResult)>;
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
  // Raw HTTP syntax is parsed by admin_defaults_contract first. These methods
  // revalidate native typed inputs and preserve original pre-dispatch depth.
  // Every operation refreshes actual kernel.admin; logical choices are not
  // target grants and need no eligibility/persistence proof. Mutation503/409
  // after dispatch can mean committed but unacknowledged; never auto-retry.
  auto read_admin_defaults(
      admin_defaults::ReadRequest request, auth::AuthContext auth,
      std::string trusted_ip, int caller_depth, AdminCompletion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr) -> void;
  auto put_admin_default(
      admin_defaults::PutRequest request, auth::AuthContext auth,
      std::string trusted_ip, int caller_depth, AdminCompletion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr) -> void;
  auto delete_admin_default(
      admin_defaults::DeleteRequest request, auth::AuthContext auth,
      std::string trusted_ip, int caller_depth, AdminCompletion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr) -> void;
  auto close_admission() -> void;
  // Waits actual worker, callback and native descendant ownership, not just JS
  // completion. A failed drain must remain visible to the process coordinator.
  [[nodiscard]] auto drain(std::chrono::steady_clock::time_point deadline)
      -> bool;
  [[nodiscard]] auto active_count() const -> std::size_t;

 private:
  friend auto test_seam::fixed_storage_dispatch_attempts(
      const FloatLayoutService&) -> std::size_t;
  friend auto test_seam::set_admin_native_settled_hook(FloatLayoutService&,
                                                       std::function<void()>)
      -> void;
  friend auto test_seam::admin_dispatch_caller_depth(const FloatLayoutService&)
      -> int;
  struct State;
  struct RequestData;
  struct ReadOperation;
  struct TargetProof;
  auto submit(std::unique_ptr<RequestData> request, auth::AuthContext auth,
              std::string trusted_ip, int caller_depth, Completion completion,
              std::shared_ptr<lifecycle::AsyncTaskLease> accepted) -> void;
  auto reject_setup(Completion completion,
                    AssetCompletion asset_completion = {},
                    AdminCompletion admin_completion = {}) noexcept -> void;
  static auto prepare_admin_reply(std::shared_ptr<ReadOperation> operation,
                                  std::shared_ptr<PGconn> connection) -> void;
  static auto final_admin_fence(std::shared_ptr<ReadOperation> operation,
                                std::shared_ptr<PGconn> connection) -> bool;
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
