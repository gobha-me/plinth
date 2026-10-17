#pragma once

#include "kernel/packages/restore_policy.hpp"

#include <expected>
#include <optional>
#include <string_view>
#include <utility>

namespace plinth::packages::restore {

enum class PolicyLoadError { UNPROTECTED, UNAVAILABLE, CHANGED, INVALID };

class ProtectedPolicy;
[[nodiscard]] auto load_protected_policy(std::string_view absolute_path)
    -> std::expected<std::optional<ProtectedPolicy>, PolicyLoadError>;

class ProtectedPolicy {
 public:
  [[nodiscard]] auto parsed() const -> const ParsedPolicy& { return parsed_; }

 private:
  explicit ProtectedPolicy(ParsedPolicy parsed) : parsed_(std::move(parsed)) {}
  ParsedPolicy parsed_;
  friend auto load_protected_policy(std::string_view)
      -> std::expected<std::optional<ProtectedPolicy>, PolicyLoadError>;
};

// Linux deployment profile: root-owned regular file and root-owned ancestors,
// no symlinks or group/other writes. The service must be non-root, without
// permitted/effective capabilities, and already have no_new_privs set. This
// excludes same-OS-identity policy, including a service-owned chmod 0444 file.
// Empty configuration is affirmative absence; a configured failure is NOT.
// Call once during owned startup, before ingress. No watching or file paths
// escape. Parsing/ownership does not establish the reviewer's current RBAC.
[[nodiscard]] auto load_protected_policy(std::string_view absolute_path)
    -> std::expected<std::optional<ProtectedPolicy>, PolicyLoadError>;

} // namespace plinth::packages::restore
