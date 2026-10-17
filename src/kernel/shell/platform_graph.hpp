#pragma once

#include "kernel/packages/generation_cache.hpp"

#include <expected>
#include <memory>
#include <string>

namespace plinth::packages::generation {
class CapturedArtifact;
}

namespace plinth::shell {

enum class PlatformGraphError { INCOMPATIBLE, TOO_LARGE, BUSY, HASHING_FAILED };
struct PlatformDocumentStorage;

// Small immutable bootstrap bytes only. This holds neither a full captured
// artifact nor a RuntimePool owner. Client-asset transmission needs its own
// freshly admitted physical artifact lease; this metadata is not authority.
class PreparedPlatform {
 public:
  ~PreparedPlatform();
  PreparedPlatform(const PreparedPlatform&) = delete;
  auto operator=(const PreparedPlatform&) -> PreparedPlatform& = delete;
  [[nodiscard]] auto key() const noexcept
      -> const packages::generation::GenerationKey&;
  [[nodiscard]] auto artifact_digest() const noexcept -> const std::string&;
  [[nodiscard]] auto client_graph_digest() const noexcept -> const std::string&;
  [[nodiscard]] auto profile() const noexcept -> const std::string&;
  [[nodiscard]] auto base() const noexcept -> const std::string&;
  [[nodiscard]] auto document() const noexcept -> const std::string&;
  [[nodiscard]] auto policy() const noexcept -> const std::string&;

 private:
  friend auto prepare_platform(const packages::generation::CapturedArtifact&,
                               packages::generation::Budget&)
      -> std::expected<std::shared_ptr<const PreparedPlatform>,
                       PlatformGraphError>;
  explicit PreparedPlatform(std::unique_ptr<PlatformDocumentStorage> storage);
  std::unique_ptr<PlatformDocumentStorage> storage;
};

// Pure bounded preparation from immutable bytes and this build's generated
// trusted complete CLIENT_FILES. No DB/IO/cache publication and no permissions.
// Fails closed on any extra/missing client member or mismatched shipped pin.
[[nodiscard]] auto prepare_platform(
    const packages::generation::CapturedArtifact& artifact,
    packages::generation::Budget& budget)
    -> std::expected<std::shared_ptr<const PreparedPlatform>,
                     PlatformGraphError>;

} // namespace plinth::shell
