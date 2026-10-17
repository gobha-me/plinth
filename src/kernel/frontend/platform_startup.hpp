#pragma once

#include "kernel/config.hpp"
#include "kernel/shell/active_frontend.hpp"

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>

namespace plinth::packages::generation {
class InstalledGenerationRegistry;
}

namespace plinth::frontend {

enum class PlatformStartupError { UNAVAILABLE, INTERRUPTED, RECOVERY_REQUIRED };

// Owned pre-ingress startup only. Resolves one authoritative frontend; custom
// and incompatible profiles remain ordinary frontends without typed approval.
// For the compiled bundled profile, establishes a real same-name receipt,
// acknowledged migration/source/pool binding and immutable document
// publication. This owns its session/name/fence/transaction; no thread or route
// is created.
[[nodiscard]] auto prepare_platform_startup(
    const Config& config,
    const std::shared_ptr<packages::generation::InstalledGenerationRegistry>&
        registry,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token cancellation = {})
    -> std::expected<std::optional<shell::ActiveFrontend>,
                     PlatformStartupError>;

} // namespace plinth::frontend
