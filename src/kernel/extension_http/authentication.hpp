#pragma once

#include "kernel/auth/middleware.hpp"
#include "kernel/extension_http/ownership.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace plinth::extension_http {

struct AuthenticationRun;

// Selected by the kernel from the captured committed claim, NEVER request,
// extension or client flags. This wrapper does not perform that lookup.
enum class AuthenticationMode { PROTECTED, PUBLIC };
enum class AuthenticationFailureCode { REJECTED, RETIRED, UNAVAILABLE };
struct AuthenticationFailure {
  AuthenticationFailureCode code;
  // Retired/cancelled/expired completion has no response; the common transport
  // owner decides its existing bounded wire outcome. This is never a permit.
  drogon::HttpResponsePtr response;
};

// Credential-free timing/cancellation owner. Create AFTER feature admission,
// BEFORE authentication/extra copies. It creates no timer/thread: the common
// owned request deadline source calls expire/cancel and retains this owner.
// Clock must be thread-safe, own its captures, and not re-enter this owner.
class AuthenticationOwner {
 public:
  using Clock = ResponseArbiter::Clock;
  [[nodiscard]] static auto create(std::shared_ptr<AdmissionLease> admission,
                                   DeadlineBounds bounds, Clock clock,
                                   std::stop_token stop = {})
      -> std::shared_ptr<AuthenticationOwner>;
  ~AuthenticationOwner();
  AuthenticationOwner(const AuthenticationOwner&) = delete;
  auto operator=(const AuthenticationOwner&) -> AuthenticationOwner& = delete;
  [[nodiscard]] auto current() const noexcept -> bool;
  auto cancel(CancelReason reason) noexcept -> void;
  auto expire() noexcept -> void;

 private:
  friend struct AuthenticationRun;
  struct State;
  explicit AuthenticationOwner(std::unique_ptr<State> state);
  [[nodiscard]] auto reserve(std::size_t bytes)
      -> std::optional<ExtraMemoryCharge>;
  std::unique_ptr<State> state;
};

// Actual wrapper output, not publicly constructible or copyable. It retains
// only IDs/anonymous plus credential-free owner/charge, never request, token,
// token hash, username, cookie, filter context or an authentication callback.
// snapshot() rechecks live timing/cancellation even if a completion selected
// just before cancellation is delivered later. Returned Principal is DATA;
// every later async/durable/adapter handoff still needs its own live owner
// gate. Callers reserve their own credits before retaining additional DTO
// copies.
class AuthenticatedHttpPrincipal {
 public:
  AuthenticatedHttpPrincipal(AuthenticatedHttpPrincipal&&) noexcept = default;
  auto operator=(AuthenticatedHttpPrincipal&&) noexcept
      -> AuthenticatedHttpPrincipal& = default;
  AuthenticatedHttpPrincipal(const AuthenticatedHttpPrincipal&) = delete;
  auto operator=(const AuthenticatedHttpPrincipal&)
      -> AuthenticatedHttpPrincipal& = delete;
  [[nodiscard]] auto snapshot() const -> std::optional<Principal>;

 private:
  friend struct AuthenticationRun;
  AuthenticatedHttpPrincipal(Principal principal,
                             std::shared_ptr<AuthenticationOwner> owner,
                             ExtraMemoryCharge allocation);
  Principal principal;
  std::shared_ptr<AuthenticationOwner> owner;
  ExtraMemoryCharge allocation;
};

using AuthenticationResult =
    std::expected<AuthenticatedHttpPrincipal, AuthenticationFailure>;
using AuthenticationCompletion = std::function<void(AuthenticationResult)>;

// True means an owned operation accepted completion responsibility (possibly
// synchronous); false means no filter/auth work started and no callback. The
// caller's existing transport owner handles resource rejection. Completion must
// own its captures; it is a candidate/result, NEVER permission to write/replay.
// Uses actual SessionFilter for protected, actual PublicOriginFilter only for
// unsafe public requests. No changes to global auth/CSRF/origin selection.
[[nodiscard]] auto authenticate_http(const drogon::HttpRequestPtr& request,
                                     AuthenticationMode mode,
                                     std::shared_ptr<AuthenticationOwner> owner,
                                     AuthenticationCompletion completion)
    -> bool;

namespace test_seam {
// Production decision path with the EXISTING SessionFilter validator seam.
// Hermetic validator results are NOT real credential/PG/route proof. Production
// entry above never accepts a replaceable validator or registration callback.
[[nodiscard]] auto authenticate_http(const drogon::HttpRequestPtr& request,
                                     AuthenticationMode mode,
                                     std::shared_ptr<AuthenticationOwner> owner,
                                     AuthenticationCompletion completion,
                                     auth::test_seam::TokenValidator validator)
    -> bool;
} // namespace test_seam

} // namespace plinth::extension_http
