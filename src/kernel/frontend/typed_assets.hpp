#pragma once

#include "kernel/capabilities/drain.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/packages/generation_capture.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace plinth::frontend {

class FloatLayoutService;
class TypedAssetStreams;
enum class AssetStreamError { INVALID, BUSY, CLOSED, LIMIT_EXCEEDED, RANGE };
using AssetSource = std::function<std::size_t(char*, std::size_t)>;

// Constructed only after fresh service authority/receipt validation. This token
// does not itself perform or substitute for that validation.
class AdmittedAsset {
 public:
  AdmittedAsset(AdmittedAsset&&) noexcept = default;
  auto operator=(AdmittedAsset&& other) noexcept -> AdmittedAsset&;
  AdmittedAsset(const AdmittedAsset&) = delete;
  auto operator=(const AdmittedAsset&) -> AdmittedAsset& = delete;

 private:
  friend class FloatLayoutService;
  friend class TypedAssetStreams;
  AdmittedAsset(
      std::shared_ptr<const packages::generation::CapturedArtifact> artifact,
      std::string_view member,
      std::unique_ptr<capabilities::drain::DispatchGuard> guard,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted);
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  std::shared_ptr<const packages::generation::CapturedArtifact> artifact;
  const packages::restore::ArtifactRecord* member = nullptr;
  std::unique_ptr<capabilities::drain::DispatchGuard> guard;
};

namespace typed_assets::test_seam {
// Low-level source ownership only: neither authorization nor a native HTTP
// journey. There is deliberately no test factory for AdmittedAsset.
[[nodiscard]] auto make_source(
    TypedAssetStreams& streams,
    std::shared_ptr<const packages::generation::CapturedArtifact> artifact,
    std::string_view member,
    std::unique_ptr<capabilities::drain::DispatchGuard> guard,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted,
    std::weak_ptr<trantor::TcpConnection> connection = {})
    -> std::expected<AssetSource, AssetStreamError>;
// SAME production bodyless serializer helper, without an admission token or
// source owner. Tests use this only to inspect pinned framework serialization.
[[nodiscard]] auto bodyless_response(std::string_view member,
                                     const std::string& quoted_etag,
                                     std::size_t representation_size,
                                     bool not_modified)
    -> drogon::HttpResponsePtr;
} // namespace typed_assets::test_seam

// At most 64 physical source owners, no waiting queue. Framework transport/TLS
// buffers are outside the captured-byte budget. Destruction requests stop but
// cannot assert settlement: production must explicitly drain under its owning
// shutdown deadline and retain failed owners until the watchdog boundary.
class TypedAssetStreams {
 public:
  explicit TypedAssetStreams(packages::generation::Budget budget);
  ~TypedAssetStreams();
  TypedAssetStreams(const TypedAssetStreams&) = delete;
  auto operator=(const TypedAssetStreams&) -> TypedAssetStreams& = delete;
  [[nodiscard]] auto make_response(const drogon::HttpRequestPtr& request,
                                   AdmittedAsset admitted)
      -> std::expected<drogon::HttpResponsePtr, AssetStreamError>;
  auto close_admission() -> void;
  [[nodiscard]] auto request_stop() -> bool;
  [[nodiscard]] auto drain(std::chrono::steady_clock::time_point deadline)
      -> bool;
  [[nodiscard]] auto active_count() const -> std::size_t;

 private:
  friend auto typed_assets::test_seam::make_source(
      TypedAssetStreams&,
      std::shared_ptr<const packages::generation::CapturedArtifact>,
      std::string_view, std::unique_ptr<capabilities::drain::DispatchGuard>,
      std::shared_ptr<lifecycle::AsyncTaskLease>,
      std::weak_ptr<trantor::TcpConnection>)
      -> std::expected<AssetSource, AssetStreamError>;
  struct State;
  struct SendOwner;
  [[nodiscard]] auto source(
      std::shared_ptr<const packages::generation::CapturedArtifact> artifact,
      const packages::restore::ArtifactRecord* member,
      std::unique_ptr<capabilities::drain::DispatchGuard> guard,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted,
      std::weak_ptr<trantor::TcpConnection> connection)
      -> std::expected<AssetSource, AssetStreamError>;
  std::shared_ptr<State> state;
};

} // namespace plinth::frontend
