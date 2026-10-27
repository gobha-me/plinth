#include "kernel/packages/http_generation.hpp"

#include "kernel/packages/detail/launcher_validation.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace plinth::packages::http_generation {
namespace {
constexpr std::string_view ENTRY = "server/http.js";
// Explicit index/control-block allocation envelope, independent of captured
// source storage. Returned views copy shared owners without allocating bytes.
constexpr std::size_t INDEX_METADATA_BYTES = 4096;

struct ResolvedPath {
  std::array<char, restore::INVENTORY_PATH_MAX_BYTES> bytes{};
  std::size_t size = 0;
  [[nodiscard]] auto view() const -> std::string_view {
    return {bytes.data(), size};
  }
};

auto resolve(std::string_view importer, std::string_view specifier)
    -> std::expected<ResolvedPath, ModuleError> {
  if (specifier.empty() ||
      specifier.size() > restore::INVENTORY_PATH_MAX_BYTES ||
      !(specifier.starts_with("./") || specifier.starts_with("../")) ||
      !detail::unicode_scalar_count(specifier) ||
      std::ranges::any_of(specifier, [](char byte) {
        const auto value = static_cast<unsigned char>(byte);
        return value < 0x20U || value == 0x7fU || byte == '\\';
      }))
    return std::unexpected(ModuleError::IMPORT_REJECTED);

  ResolvedPath result;
  std::array<std::size_t, restore::INVENTORY_MAX_COMPONENTS> ends{};
  std::size_t depth = 0;
  const auto directory = importer.substr(0, importer.rfind('/'));
  if ((directory != "server" && !directory.starts_with("server/")) ||
      directory.size() > result.bytes.size())
    return std::unexpected(ModuleError::IMPORT_REJECTED);
  std::ranges::copy(directory, result.bytes.begin());
  result.size = directory.size();
  for (std::size_t index = 0; index < directory.size(); ++index) {
    if (directory[index] != '/') continue;
    if (depth == ends.size())
      return std::unexpected(ModuleError::IMPORT_REJECTED);
    ends[depth++] = index;
  }
  if (depth == ends.size())
    return std::unexpected(ModuleError::IMPORT_REJECTED);
  ends[depth++] = directory.size();

  std::size_t start = 0;
  while (start < specifier.size()) {
    const auto slash = specifier.find('/', start);
    const auto end = slash == std::string_view::npos ? specifier.size() : slash;
    const auto component = specifier.substr(start, end - start);
    if (component.empty()) return std::unexpected(ModuleError::IMPORT_REJECTED);
    if (component == "..") {
      if (depth <= 1) return std::unexpected(ModuleError::IMPORT_REJECTED);
      --depth;
      result.size = ends[depth - 1];
    } else if (component != ".") {
      if (depth == ends.size() ||
          component.size() + 1 > result.bytes.size() - result.size)
        return std::unexpected(ModuleError::IMPORT_REJECTED);
      result.bytes[result.size++] = '/';
      std::ranges::copy(component,
                        result.bytes.begin() +
                            static_cast<std::ptrdiff_t>(result.size));
      result.size += component.size();
      ends[depth++] = result.size;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
    if (start == specifier.size())
      return std::unexpected(ModuleError::IMPORT_REJECTED);
  }
  return result;
}
} // namespace

struct ModuleStorage {
  generation::BudgetTicket metadata;
  std::shared_ptr<const generation::CapturedArtifact> artifact;
  const restore::ArtifactRecord* entry;
};

HttpModuleSource::HttpModuleSource(std::shared_ptr<const ModuleStorage> storage,
                                   const restore::ArtifactRecord* record)
    : storage(std::move(storage)), record(record) {
}

auto HttpModuleSource::path() const noexcept -> std::string_view {
  return record->path;
}
auto HttpModuleSource::bytes() const noexcept -> std::string_view {
  return record->content;
}

HttpModuleIndex::HttpModuleIndex(std::shared_ptr<const ModuleStorage> storage)
    : storage(std::move(storage)) {
}

auto HttpModuleIndex::create(
    std::shared_ptr<const generation::CapturedArtifact> artifact,
    generation::Budget& budget) -> std::expected<HttpModuleIndex, ModuleError> {
  if (!artifact || artifact->key().profile != 1 ||
      !artifact->belongs_to(budget))
    return std::unexpected(ModuleError::INVALID_CAPTURE);
  const auto* entry = artifact->find(ENTRY);
  if (entry == nullptr || entry->kind != restore::ArtifactKind::REGULAR)
    return std::unexpected(ModuleError::ENTRY_REQUIRED);
  auto ticket = budget.try_reserve(INDEX_METADATA_BYTES);
  if (!ticket) return std::unexpected(ModuleError::BUDGET_UNAVAILABLE);
  try {
    auto storage = std::make_shared<ModuleStorage>(std::move(*ticket),
                                                   std::move(artifact), entry);
    return HttpModuleIndex{std::move(storage)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(ModuleError::UNAVAILABLE);
  }
}

auto HttpModuleIndex::entry() const -> HttpModuleSource {
  return {storage, storage->entry};
}

auto HttpModuleIndex::resolve_import(const HttpModuleSource& importer,
                                     std::string_view specifier) const
    -> std::expected<HttpModuleSource, ModuleError> {
  if (importer.storage != storage)
    return std::unexpected(ModuleError::IMPORT_REJECTED);
  const auto path = resolve(importer.path(), specifier);
  if (!path) return std::unexpected(path.error());
  const auto* record = storage->artifact->find(path->view());
  if (record == nullptr || record->kind != restore::ArtifactKind::REGULAR)
    return std::unexpected(ModuleError::MODULE_MISSING);
  return HttpModuleSource{storage, record};
}

auto HttpModuleIndex::artifact() const noexcept
    -> const std::shared_ptr<const generation::CapturedArtifact>& {
  return storage->artifact;
}

} // namespace plinth::packages::http_generation
