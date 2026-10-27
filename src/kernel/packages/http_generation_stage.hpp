#pragma once

#include "kernel/packages/http_generation.hpp"
#include "kernel/packages/http_install_authorization.hpp"
#include "kernel/packages/installed_generation_registry.hpp"

#include <expected>
#include <optional>

namespace plinth::packages::http_generation {

enum class PreparationError {
  INVALID,
  SOURCE_MISMATCH,
  ENTRY_REQUIRED,
  BUDGET_UNAVAILABLE,
  UNAVAILABLE,
};

// Owns real staged bytes and the matching opaque install operation, not an
// HTTP receipt, observed COMMIT, compiled companion, adapter or publication.
// All caller transaction/acknowledgement and real mode-factory gates remain.
class PreparedHttpCapture {
 public:
  PreparedHttpCapture(PreparedHttpCapture&&) noexcept = default;
  auto operator=(PreparedHttpCapture&& other) noexcept -> PreparedHttpCapture&;
  PreparedHttpCapture(const PreparedHttpCapture&) = delete;
  auto operator=(const PreparedHttpCapture&) -> PreparedHttpCapture& = delete;
  [[nodiscard]] auto prepared_generation() const noexcept
      -> const generation::PreparedGeneration&;
  [[nodiscard]] auto authorization() const noexcept
      -> const AuthorizedHttpSubmission&;
  [[nodiscard]] auto modules() const noexcept
      -> const std::optional<HttpModuleIndex>&;

 private:
  friend auto prepare_http_capture(generation::PreparedGeneration,
                                   AuthorizedHttpSubmission,
                                   generation::Budget&)
      -> std::expected<PreparedHttpCapture, PreparationError>;
  PreparedHttpCapture(generation::BudgetTicket, generation::PreparedGeneration,
                      AuthorizedHttpSubmission, std::optional<HttpModuleIndex>);
  generation::BudgetTicket metadata;
  generation::PreparedGeneration prepared;
  AuthorizedHttpSubmission authorized;
  std::optional<HttpModuleIndex> module_index;
};

// Synchronous no-I/O binding, called after actual stage_locked and BEFORE
// acquiring the global claim guard. Checks exact manifest bytes against the
// authorization, name/version and private staged artifact identity. The
// immutable capture includes every file; only QuickJS needs server/http.js.
// No user code is compiled/evaluated and no mode is declared physically ready.
// Consumes both owners even on refusal; caller still owns its PG transaction.
[[nodiscard]] auto prepare_http_capture(generation::PreparedGeneration prepared,
                                        AuthorizedHttpSubmission authorization,
                                        generation::Budget& budget)
    -> std::expected<PreparedHttpCapture, PreparationError>;

} // namespace plinth::packages::http_generation
