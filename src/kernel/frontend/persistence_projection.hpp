#pragma once

#include "kernel/packages/panels_manifest.hpp"
#include "kernel/packages/protected_restore_policy.hpp"
#include "kernel/packages/restore_codec.hpp"

#include <nlohmann/json_fwd.hpp>

#include <exception>
#include <expected>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::frontend {

// An operational unknown is never affirmative ineligibility. The owned worker
// translates this allocation-free, redacted exception into whole-request503.
class ProjectionUnavailable final : public std::exception {
 public:
  [[nodiscard]] auto what() const noexcept -> const char* override {
    return "persistence projection unavailable";
  }
};

namespace projection_detail {
// Pure error-boundary machinery, not a policy/authority/token factory or fault
// injection hook. Focused tests exercise the SAME production classification.
[[nodiscard]] auto checked_codec(
    std::expected<std::string, packages::restore::CodecError> result)
    -> std::optional<std::string>;

template <typename Operation>
auto operational_boundary(Operation&& operation)
    -> decltype(std::forward<Operation>(operation)()) {
  try {
    return std::forward<Operation>(operation)();
  } catch (const std::bad_alloc&) {
    throw ProjectionUnavailable{};
  } catch (const std::length_error&) {
    throw ProjectionUnavailable{};
  }
}
} // namespace projection_detail

struct PersistenceProjection {
  packages::restore::PolicyProjection projection_contract;
  std::string artifact_digest;
  std::string schema_digest;
  std::string policy_revision;
  std::vector<std::string> approved_keys;
};

// Semantic matching only, never an admission/write token. The service still
// needs fresh credential, reviewer eligibility, RBAC, receipt and lifecycle
// checks. The protected snapshot cannot be constructed from caller JSON.
// Operational codec/hash/allocation failure throws ProjectionUnavailable;
// nullopt/false below remain semantic mismatch/invalid-context results only.
[[nodiscard]] auto match_persistence_projection(
    const packages::restore::ProtectedPolicy& policy,
    std::string_view application, std::string_view panel,
    std::string_view capability, std::string_view artifact_digest,
    const packages::PanelContextSchema& schema)
    -> std::optional<PersistenceProjection>;

[[nodiscard]] auto validate_float_context(
    const packages::PanelContextSchema& schema, const nlohmann::json& context)
    -> bool;

// Produces only the selected minimal stable fields. Unknown/invalid fields,
// a changed schema or a required reopen field lost by projection fail closed.
[[nodiscard]] auto project_float_context(
    const PersistenceProjection& projection,
    const packages::PanelContextSchema& schema, const nlohmann::json& context)
    -> std::optional<nlohmann::json>;

} // namespace plinth::frontend
