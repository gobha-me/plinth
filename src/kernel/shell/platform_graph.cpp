#include "kernel/shell/platform_graph.hpp"

#include "kernel/packages/generation_capture.hpp"
#include "kernel/packages/migrations_internal.hpp"
#include "kernel/packages/restore_digest.hpp"
#include "plinth/platform_profile.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::shell {
namespace {

constexpr std::string_view BASE_MARKER = "<!-- PLINTH_VERSIONED_ASSET_BASE -->";
constexpr std::string_view MAP_BEGIN =
    "<!-- PLINTH_PLATFORM_IMPORT_MAP_BEGIN -->";
constexpr std::string_view MAP_END = "<!-- PLINTH_PLATFORM_IMPORT_MAP_END -->";
constexpr std::string_view ENTRY_MODULE =
    "<script type=\"module\" src=\"shell.js\"></script>";
constexpr std::size_t TEMPLATE_MAX = std::size_t{64} * 1024;
constexpr std::size_t PREPARATION_BYTES = std::size_t{256} * 1024;
constexpr std::string_view POLICY_SUFFIX =
    "'; default-src 'self'; style-src 'self' 'unsafe-inline'; "
    "connect-src 'self'; img-src 'self'; font-src 'self'; media-src 'self'; "
    "frame-src 'self'; frame-ancestors 'self'; base-uri 'self'; "
    "form-action 'self'; object-src 'none'";

auto hex(std::string_view value) -> bool {
  return std::ranges::all_of(value, [](char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
  });
}

auto canonical_uuid(std::string_view value) -> bool {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!hex(value.substr(index, 1))) {
      return false;
    }
  }
  return true;
}

auto unique_position(std::string_view body, std::string_view marker)
    -> std::optional<std::size_t> {
  const auto at = body.find(marker);
  if (at == std::string_view::npos ||
      body.find(marker, at + marker.size()) != std::string_view::npos)
    return std::nullopt;
  return at;
}

auto csp_digest(std::string_view bytes) -> std::string {
  std::array<unsigned char, 32> digest{};
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1 ||
      length != digest.size()) {
    throw std::runtime_error("platform.graph.hash");
  }
  std::array<unsigned char, 45> encoded{};
  if (EVP_EncodeBlock(encoded.data(), digest.data(),
                      static_cast<int>(digest.size())) != 44) {
    throw std::runtime_error("platform.graph.hash");
  }
  return {reinterpret_cast<const char*>(encoded.data()), 44};
}

auto exact_pinned_file(
    const packages::generation::CapturedArtifact& artifact,
    std::string_view path, std::string_view hash,
    std::size_t cap = packages::restore::INVENTORY_CONTENT_MAX_BYTES) -> bool {
  const auto* file = artifact.find(path);
  return file && file->kind == packages::restore::ArtifactKind::REGULAR &&
         file->content.size() <= cap &&
         packages::detail::sha256_hex(file->content) == hash;
}

auto client_inventory(const packages::generation::CapturedArtifact& artifact)
    -> std::expected<std::string, PlatformGraphError> {
  for (const auto& expected : platform_profile::CLIENT_FILES) {
    if (!exact_pinned_file(artifact, expected.path, expected.sha256)) {
      return std::unexpected(PlatformGraphError::INCOMPATIBLE);
    }
  }
  std::vector<packages::restore::ArtifactRecord> records;
  records.reserve(artifact.records().size());
  std::size_t file_count = 0;
  for (const auto& record : artifact.records()) {
    if (record.path != "client" && !record.path.starts_with("client/"))
      continue;
    if (record.kind == packages::restore::ArtifactKind::DIRECTORY) {
      // Only directories implied by shipped files are allowed, including
      // client itself. No copied path set or hidden empty-directory extension.
      const bool expected_directory = std::ranges::any_of(
          platform_profile::CLIENT_FILES, [&](const auto& file) {
            return file.path.size() > record.path.size() &&
                   file.path.starts_with(record.path) &&
                   file.path[record.path.size()] == '/';
          });
      if (!expected_directory) {
        return std::unexpected(PlatformGraphError::INCOMPATIBLE);
      }
    } else {
      const auto expected =
          std::ranges::find(platform_profile::CLIENT_FILES, record.path,
                            &platform_profile::FileDigest::path);
      if (expected == platform_profile::CLIENT_FILES.end()) {
        return std::unexpected(PlatformGraphError::INCOMPATIBLE);
      }
      ++file_count;
    }
    records.push_back(record);
  }
  if (file_count != platform_profile::CLIENT_FILES.size()) {
    return std::unexpected(PlatformGraphError::INCOMPATIBLE);
  }
  auto digest = packages::restore::artifact_digest(records);
  if (!digest) return std::unexpected(PlatformGraphError::INCOMPATIBLE);
  return std::move(*digest);
}

} // namespace

struct PlatformDocumentStorage {
  packages::generation::BudgetTicket charge;
  packages::generation::GenerationKey key;
  std::string artifact_digest;
  std::string client_digest;
  std::string profile;
  std::string base;
  std::string document;
  std::string policy;
};

PreparedPlatform::PreparedPlatform(
    std::unique_ptr<PlatformDocumentStorage> owned)
    : storage(std::move(owned)) {
}
PreparedPlatform::~PreparedPlatform() = default;
auto PreparedPlatform::key() const noexcept
    -> const packages::generation::GenerationKey& {
  return storage->key;
}
auto PreparedPlatform::artifact_digest() const noexcept -> const std::string& {
  return storage->artifact_digest;
}
auto PreparedPlatform::client_graph_digest() const noexcept
    -> const std::string& {
  return storage->client_digest;
}
auto PreparedPlatform::profile() const noexcept -> const std::string& {
  return storage->profile;
}
auto PreparedPlatform::base() const noexcept -> const std::string& {
  return storage->base;
}
auto PreparedPlatform::document() const noexcept -> const std::string& {
  return storage->document;
}
auto PreparedPlatform::policy() const noexcept -> const std::string& {
  return storage->policy;
}

auto prepare_platform(const packages::generation::CapturedArtifact& artifact,
                      packages::generation::Budget& budget)
    -> std::expected<std::shared_ptr<const PreparedPlatform>,
                     PlatformGraphError> try {
  const auto& key = artifact.key();
  if (key.name != "shell" || key.profile != 1 ||
      !canonical_uuid(key.package_id) || artifact.digest().size() != 64 ||
      !hex(artifact.digest()) ||
      !exact_pinned_file(artifact, "server/platform-storage-v1.js",
                         platform_profile::MODULE_SHA256,
                         std::size_t{32} * 1024) ||
      !exact_pinned_file(artifact, "server/plinth-platform.json",
                         platform_profile::CONTRACT_SHA256, 1024) ||
      !exact_pinned_file(artifact, "migrations/002_platform_storage_v1.sql",
                         platform_profile::MIGRATION_SHA256,
                         std::size_t{8} * 1024)) {
    return std::unexpected(PlatformGraphError::INCOMPATIBLE);
  }
  auto charge = budget.try_reserve(PREPARATION_BYTES);
  if (!charge) return std::unexpected(PlatformGraphError::BUSY);
  auto client_digest = client_inventory(artifact);
  if (!client_digest) return std::unexpected(client_digest.error());
  const auto* entry = artifact.find("client/index.html");
  if (!entry || entry->content.size() > TEMPLATE_MAX) {
    return std::unexpected(PlatformGraphError::TOO_LARGE);
  }
  const auto base_at = unique_position(entry->content, BASE_MARKER);
  const auto begin_at = unique_position(entry->content, MAP_BEGIN);
  const auto end_at = unique_position(entry->content, MAP_END);
  // This compiled trusted template documents module syntax inside an earlier
  // HTML comment. Match its actual exact entry tag, not a comment's example.
  const auto module_at = unique_position(entry->content, ENTRY_MODULE);
  if (!base_at || !begin_at || !end_at || *base_at >= *begin_at ||
      *begin_at >= *end_at || !module_at ||
      *end_at + MAP_END.size() >= *module_at) {
    return std::unexpected(PlatformGraphError::INCOMPATIBLE);
  }
  auto owned = std::make_unique<PlatformDocumentStorage>();
  owned->charge = std::move(*charge);
  owned->key = key;
  owned->artifact_digest = artifact.digest();
  owned->client_digest = std::move(*client_digest);
  owned->profile = "v1:" + key.package_id + ":" + artifact.digest();
  owned->base = "/_plinth/platform/" + key.package_id + "/" +
                artifact.digest() + "/client/";
  const auto& base = owned->base;
  const auto map = "{\"imports\":{\"@plinth/frontend/sdk\":\"" + base +
                   "sdk.js\"," + "\"@plinth/frontend/tokens\":\"" + base +
                   "css/tokens.css\"," + "\"preact\":\"" + base +
                   "vendor/preact.module.js\"," + "\"preact/hooks\":\"" + base +
                   "vendor/preact-hooks.module.js\"," + "\"htm\":\"" + base +
                   "vendor/htm.module.js\"}}";
  owned->policy = "script-src 'self' 'sha256-" + csp_digest(map) +
                  std::string{POLICY_SUFFIX};
  owned->document = entry->content;
  owned->document.replace(*begin_at, *end_at + MAP_END.size() - *begin_at,
                          "<script type=\"importmap\">" + map + "</script>");
  owned->document.replace(
      *base_at, BASE_MARKER.size(),
      "<base href=\"" + base +
          "\"><meta name=\"plinth-platform-profile\" content=\"" +
          owned->profile + "\">");
  if (owned->document.size() > TEMPLATE_MAX) {
    return std::unexpected(PlatformGraphError::TOO_LARGE);
  }
  const auto resident =
      sizeof(PreparedPlatform) + sizeof(PlatformDocumentStorage) +
      owned->key.package_id.capacity() + owned->key.name.capacity() +
      owned->key.version.capacity() + owned->artifact_digest.capacity() +
      owned->client_digest.capacity() + owned->profile.capacity() +
      owned->base.capacity() + owned->document.capacity() +
      owned->policy.capacity() + 9;
  if (resident > PREPARATION_BYTES)
    return std::unexpected(PlatformGraphError::TOO_LARGE);
  owned->charge.shrink(resident);
  return std::shared_ptr<const PreparedPlatform>{
      new PreparedPlatform{std::move(owned)}};
} catch (const std::bad_alloc&) {
  return std::unexpected(PlatformGraphError::BUSY);
} catch (const std::length_error&) {
  return std::unexpected(PlatformGraphError::TOO_LARGE);
} catch (const std::runtime_error&) {
  return std::unexpected(PlatformGraphError::HASHING_FAILED);
}

} // namespace plinth::shell
