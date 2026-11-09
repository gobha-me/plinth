// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/capabilities/resolution.hpp"
#include "kernel/extension_http/authentication.hpp"

#include <expected>
#include <optional>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::extension_http {

enum class CallerResolutionFailure { REJECTED, RETIRED, UNAVAILABLE, BUSY };

struct HttpCallerSnapshot {
  Principal principal;
  capabilities::UserContext user;
};

// Credential-free caller resolved from the sealed authentication output and
// PostgreSQL. The caller cannot construct or copy this owner. Retaining it
// retains the accepted process/admission lease, independently of authentication
// callback closure. Its rules are a bounded request snapshot, not a live grant.
class ResolvedHttpCaller {
 public:
  ResolvedHttpCaller(ResolvedHttpCaller&&) noexcept = default;
  auto operator=(ResolvedHttpCaller&&) noexcept
      -> ResolvedHttpCaller& = default;
  ResolvedHttpCaller(const ResolvedHttpCaller&) = delete;
  auto operator=(const ResolvedHttpCaller&) -> ResolvedHttpCaller& = delete;
  // Rechecks the original timing/cancellation gate. Returned data is never an
  // entry permit: later queue/adapter handoffs must recheck their own live gate
  // and reserve credits before retaining further copies of these bounded data.
  [[nodiscard]] auto snapshot() const -> std::optional<HttpCallerSnapshot>;

 private:
  friend struct CallerResolutionRun;
  ResolvedHttpCaller(HttpCallerSnapshot value,
                     std::shared_ptr<AuthenticationOwner> owner,
                     ExtraMemoryCharge allocation);
  HttpCallerSnapshot value;
  std::shared_ptr<AuthenticationOwner> owner;
  ExtraMemoryCharge allocation;
};

using CallerResolutionResult =
    std::expected<ResolvedHttpCaller, CallerResolutionFailure>;

// Synchronous worker-side component; NEVER call on a Drogon/event loop. The
// enclosing worker exclusively owns an idle, authoritative kernel connection
// outside a transaction and joins this call before closing it. Uses bounded
// libpq operations with the authentication and enclosing worker stop tokens and
// one inherited absolute deadline, including physical cancellation cleanup.
// Explicit owner cancellation retires output; an
// in-flight query remains owned until its bounded native
// completion/cancellation. No new thread, timer or database client is created,
// and no transaction settings are changed. Public identities resolve only
// everyone rules; PATs recheck identity state and resolve the additive
// group/everyone union, excluding orphaned rules.
[[nodiscard]] auto resolve_http_caller(AuthenticatedHttpPrincipal principal,
                                       PGconn& connection)
    -> CallerResolutionResult;

} // namespace plinth::extension_http
