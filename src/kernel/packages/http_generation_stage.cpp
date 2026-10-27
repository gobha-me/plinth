#include "kernel/packages/http_generation_stage.hpp"

#include "kernel/packages/migrations_internal.hpp"

#include <exception>
#include <utility>

namespace plinth::packages::http_generation {

PreparedHttpCapture::PreparedHttpCapture(
    generation::BudgetTicket metadata, generation::PreparedGeneration prepared,
    AuthorizedHttpSubmission authorized,
    std::optional<HttpModuleIndex> module_index)
    : metadata(std::move(metadata)), prepared(std::move(prepared)),
      authorized(std::move(authorized)), module_index(std::move(module_index)) {
}

auto PreparedHttpCapture::operator=(PreparedHttpCapture&& other) noexcept
    -> PreparedHttpCapture& {
  // Swapping keeps BOTH retired and replacement metadata charged until their
  // corresponding strings/source owners disappear, including self-move.
  using std::swap;
  swap(metadata, other.metadata);
  swap(prepared, other.prepared);
  swap(authorized, other.authorized);
  swap(module_index, other.module_index);
  return *this;
}

auto PreparedHttpCapture::prepared_generation() const noexcept
    -> const generation::PreparedGeneration& {
  return prepared;
}
auto PreparedHttpCapture::authorization() const noexcept
    -> const AuthorizedHttpSubmission& {
  return authorized;
}
auto PreparedHttpCapture::modules() const noexcept
    -> const std::optional<HttpModuleIndex>& {
  return module_index;
}

auto prepare_http_capture(generation::PreparedGeneration prepared,
                          AuthorizedHttpSubmission authorization,
                          generation::Budget& budget)
    -> std::expected<PreparedHttpCapture, PreparationError> try {
  const auto& artifact = prepared.artifact();
  if (!artifact || !artifact->belongs_to(budget) ||
      artifact->key().profile != 1 ||
      authorization.declaration().http_prefixes.empty() ||
      !authorization.declaration().handler_mode)
    return std::unexpected(PreparationError::INVALID);
  const auto& receipt = prepared.receipt();
  const auto* manifest = artifact->find("manifest.json");
  if (receipt.key != artifact->key() ||
      receipt.artifact_digest != artifact->digest() ||
      authorization.package_name() != artifact->key().name ||
      authorization.package_version() != artifact->key().version ||
      manifest == nullptr || manifest->kind != restore::ArtifactKind::REGULAR)
    return std::unexpected(PreparationError::SOURCE_MISMATCH);

  // Reserve retained control strings plus the bounded canonical digest scratch
  // BEFORE hashing/serialization. Library allocator overhead is not a whole
  // process memory bound; source payload remains in its original capture.
  auto metadata = budget.try_reserve(std::size_t{128} * 1024);
  if (!metadata) return std::unexpected(PreparationError::BUDGET_UNAVAILABLE);
  if (detail::sha256_hex(manifest->content) !=
          authorization.raw_manifest_sha256() ||
      http_install_declarations_sha256(authorization.declaration()) !=
          authorization.declarations_sha256())
    return std::unexpected(PreparationError::SOURCE_MISMATCH);

  // Retained install-proof strings/canonical claim vectors fit this explicit
  // envelope (64 claims x256 bytes in each of two arrays plus bounded IDs).
  // This is source/control storage, not HTTP bodies or engine heap overhead.
  metadata->shrink(std::size_t{64} * 1024);
  std::optional<HttpModuleIndex> modules;
  if (authorization.declaration().handler_mode == HttpHandlerMode::QUICKJS) {
    auto index = HttpModuleIndex::create(artifact, budget);
    if (!index) {
      if (index.error() == ModuleError::ENTRY_REQUIRED)
        return std::unexpected(PreparationError::ENTRY_REQUIRED);
      if (index.error() == ModuleError::BUDGET_UNAVAILABLE)
        return std::unexpected(PreparationError::BUDGET_UNAVAILABLE);
      return std::unexpected(PreparationError::UNAVAILABLE);
    }
    modules.emplace(*index);
  }
  return PreparedHttpCapture{std::move(*metadata), std::move(prepared),
                             std::move(authorization), std::move(modules)};
} catch (const std::exception&) {
  return std::unexpected(PreparationError::UNAVAILABLE);
}

} // namespace plinth::packages::http_generation
