#pragma once

#include "kernel/packages/generation_capture.hpp"

#include <expected>
#include <memory>
#include <string_view>

namespace plinth::packages::http_generation {

enum class ModuleError {
  INVALID_CAPTURE,
  ENTRY_REQUIRED,
  IMPORT_REJECTED,
  MODULE_MISSING,
  BUDGET_UNAVAILABLE,
  UNAVAILABLE,
};

struct ModuleStorage;
class HttpModuleIndex;

// Length-aware immutable bytes with their physical capture AND index charge.
// No filesystem path, runtime, evaluated export or readiness authority escapes.
class HttpModuleSource {
 public:
  HttpModuleSource(const HttpModuleSource&) = default;
  auto operator=(const HttpModuleSource&) -> HttpModuleSource& = default;
  [[nodiscard]] auto path() const noexcept -> std::string_view;
  [[nodiscard]] auto bytes() const noexcept -> std::string_view;

 private:
  friend class HttpModuleIndex;
  HttpModuleSource(std::shared_ptr<const ModuleStorage> storage,
                   const restore::ArtifactRecord* record);
  std::shared_ptr<const ModuleStorage> storage;
  const restore::ArtifactRecord* record;
};

// Pure lookup over an actual full profile1 capture. Creation/import never
// evaluates, compiles, parses imports or invokes user JS. Only explicit
// relative imports inside server/ resolve; no bare/URL/absolute/filesystem
// fallback, extension guessing, percent decoding or query/fragment stripping.
// Actual engine loading and any kernel-owned builtins belong to the charged
// adapter. This value certifies neither installation/COMMIT nor physical
// readiness.
class HttpModuleIndex {
 public:
  HttpModuleIndex(const HttpModuleIndex&) = default;
  auto operator=(const HttpModuleIndex&) -> HttpModuleIndex& = default;
  [[nodiscard]] static auto create(
      std::shared_ptr<const generation::CapturedArtifact> artifact,
      generation::Budget& budget)
      -> std::expected<HttpModuleIndex, ModuleError>;
  [[nodiscard]] auto entry() const -> HttpModuleSource;
  [[nodiscard]] auto resolve_import(const HttpModuleSource& importer,
                                    std::string_view specifier) const
      -> std::expected<HttpModuleSource, ModuleError>;
  [[nodiscard]] auto artifact() const noexcept
      -> const std::shared_ptr<const generation::CapturedArtifact>&;

 private:
  explicit HttpModuleIndex(std::shared_ptr<const ModuleStorage> storage);
  std::shared_ptr<const ModuleStorage> storage;
};

} // namespace plinth::packages::http_generation
