#include "kernel/extension_http/authentication.hpp"

#include "kernel/auth/csrf.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace plinth::extension_http {
namespace {
constexpr std::size_t OWNER_BYTES = 4096;
constexpr std::size_t RUN_BYTES = 4096;
constexpr std::size_t PRINCIPAL_BYTES = 1024;
// Covers the existing selector/filter/validator raw-token copies, PAT substring
// and retained callbacks. This charges bounded payloads, not total allocator/DB
// client heap. Transport/header/cookie storage already belongs to admission.
constexpr std::size_t CREDENTIAL_COPY_FACTOR = 8;

auto safe_method(drogon::HttpMethod method) -> bool {
  return method == drogon::Get || method == drogon::Head ||
         method == drogon::Options;
}
auto response(drogon::HttpStatusCode status, const char* error,
              const char* message, bool origin_sensitive)
    -> drogon::HttpResponsePtr {
  Json::Value body;
  body["error"] = error;
  body["message"] = message;
  auto result = drogon::HttpResponse::newHttpJsonResponse(body);
  result->setStatusCode(status);
  auth::harden_auth_response(result, origin_sensitive);
  return result;
}
} // namespace

struct AuthenticationOwner::State {
  std::mutex mutex;
  std::shared_ptr<AdmissionLease> admission;
  DeadlineBounds bounds;
  Clock clock;
  std::stop_token stop;
  std::optional<CancelReason> cancelled;
  std::atomic_bool broken{false};
  ExtraMemoryCharge allocation;

  State(std::shared_ptr<AdmissionLease> admission_in, DeadlineBounds bounds_in,
        Clock clock_in, std::stop_token stop_in,
        ExtraMemoryCharge allocation_in)
      : admission(std::move(admission_in)), bounds(bounds_in),
        clock(std::move(clock_in)), stop(stop_in),
        allocation(std::move(allocation_in)) {}
};

AuthenticationOwner::AuthenticationOwner(std::unique_ptr<State> state_in)
    : state(std::move(state_in)) {
}
AuthenticationOwner::~AuthenticationOwner() = default;
auto AuthenticationOwner::create(std::shared_ptr<AdmissionLease> admission,
                                 DeadlineBounds bounds, Clock clock,
                                 std::stop_token stop)
    -> std::shared_ptr<AuthenticationOwner> try {
  auto ceiling =
      DeadlineBounds::make(bounds.admitted_at_ns, bounds.wire_expiry_ns);
  if (!admission || !clock || !ceiling ||
      bounds.execution_cutoff_ns <= bounds.admitted_at_ns ||
      bounds.execution_cutoff_ns > ceiling->execution_cutoff_ns ||
      bounds.wire_expiry_ns != ceiling->wire_expiry_ns)
    return {};
  auto charge = admission->reserve_extra(OWNER_BYTES);
  if (!charge) return {};
  auto state = std::make_unique<State>(
      std::move(admission), bounds, std::move(clock), stop, std::move(*charge));
  return std::shared_ptr<AuthenticationOwner>(
      new AuthenticationOwner(std::move(state)));
} catch (...) {
  return {};
}
auto AuthenticationOwner::current() const noexcept -> bool try {
  if (state->broken.load()) return false;
  const std::lock_guard lock(state->mutex);
  if (state->cancelled) return false;
  if (state->stop.stop_requested()) {
    state->cancelled = CancelReason::SHUTDOWN;
    return false;
  }
  const auto now = state->clock();
  if (now < state->bounds.admitted_at_ns) {
    state->cancelled = CancelReason::GENERATION_RETIRED;
    return false;
  }
  if (now >= state->bounds.execution_cutoff_ns) {
    state->cancelled = CancelReason::DEADLINE;
    return false;
  }
  return true;
} catch (...) {
  state->broken.store(true);
  return false;
}
auto AuthenticationOwner::cancel(CancelReason reason) noexcept -> void try {
  const std::lock_guard lock(state->mutex);
  if (!state->cancelled) state->cancelled = reason;
} catch (...) {
  state->broken.store(true);
}
auto AuthenticationOwner::expire() noexcept -> void {
  static_cast<void>(current());
}
auto AuthenticationOwner::reserve(std::size_t bytes)
    -> std::optional<ExtraMemoryCharge> {
  return state->admission->reserve_extra(bytes);
}

AuthenticatedHttpPrincipal::AuthenticatedHttpPrincipal(
    Principal principal_in, std::shared_ptr<AuthenticationOwner> owner_in,
    ExtraMemoryCharge allocation_in)
    : principal(std::move(principal_in)), owner(std::move(owner_in)),
      allocation(std::move(allocation_in)) {
}
auto AuthenticatedHttpPrincipal::snapshot() const -> std::optional<Principal> {
  if (!owner || !owner->current()) return std::nullopt;
  return principal;
}

// A separate credential-bearing physical callback state. The output principal
// retains AuthenticationOwner only, not this state/request/filters. Existing
// SessionFilter SELECT and last_used_at callbacks retain this state through
// their shared_cb lineage even after logical completion moves the callback out.
struct AuthenticationRun : std::enable_shared_from_this<AuthenticationRun> {
  std::mutex mutex;
  drogon::HttpRequestPtr request;
  std::shared_ptr<AuthenticationOwner> owner;
  AuthenticationCompletion completion;
  ExtraMemoryCharge allocation;
  std::optional<ExtraMemoryCharge> cookie_allocation;
  std::shared_ptr<auth::SessionFilter> session_filter;
  std::shared_ptr<auth::PublicOriginFilter> origin_filter;
  AuthenticationMode mode;
  bool origin_sensitive;
  bool completed = false;

  AuthenticationRun(drogon::HttpRequestPtr request_in,
                    std::shared_ptr<AuthenticationOwner> owner_in,
                    AuthenticationCompletion completion_in,
                    ExtraMemoryCharge allocation_in,
                    std::optional<ExtraMemoryCharge> cookie_allocation_in,
                    AuthenticationMode mode_in)
      : request(std::move(request_in)), owner(std::move(owner_in)),
        completion(std::move(completion_in)),
        allocation(std::move(allocation_in)),
        cookie_allocation(std::move(cookie_allocation_in)), mode(mode_in),
        origin_sensitive(mode == AuthenticationMode::PUBLIC &&
                         !safe_method(request->method())) {}

  auto finish(AuthenticationResult result) -> void {
    AuthenticationCompletion callback;
    {
      const std::lock_guard lock(mutex);
      if (completed) return;
      completed = true;
      callback = std::move(completion);
    }
    if (!owner->current())
      result = std::unexpected(
          AuthenticationFailure{AuthenticationFailureCode::RETIRED, {}});
    try {
      callback(std::move(result));
    } catch (...) {
      owner->cancel(CancelReason::GENERATION_RETIRED);
    }
  }
  auto deny(drogon::HttpResponsePtr rejected) -> void try {
    if (!rejected) {
      unavailable();
      return;
    }
    auth::harden_auth_response(rejected, origin_sensitive);
    finish(std::unexpected(AuthenticationFailure{
        AuthenticationFailureCode::REJECTED, std::move(rejected)}));
  } catch (...) {
    finish(std::unexpected(
        AuthenticationFailure{AuthenticationFailureCode::UNAVAILABLE, {}}));
  }
  auto unavailable() -> void try {
    auto rejected = response(
        drogon::k503ServiceUnavailable, "service_unavailable",
        "Authentication service is temporarily unavailable", origin_sensitive);
    finish(std::unexpected(AuthenticationFailure{
        AuthenticationFailureCode::UNAVAILABLE, std::move(rejected)}));
  } catch (...) {
    finish(std::unexpected(
        AuthenticationFailure{AuthenticationFailureCode::UNAVAILABLE, {}}));
  }
  auto accept(Principal principal) -> void try {
    if (!owner->current()) {
      finish(std::unexpected(
          AuthenticationFailure{AuthenticationFailureCode::RETIRED, {}}));
      return;
    }
    auto charge = owner->reserve(PRINCIPAL_BYTES);
    if (!charge) {
      deny(response(drogon::k503ServiceUnavailable, "http_surface_busy",
                    "HTTP surface is busy", origin_sensitive));
      return;
    }
    finish(AuthenticatedHttpPrincipal{std::move(principal), owner,
                                      std::move(*charge)});
  } catch (...) {
    unavailable();
  }
  auto protected_success() -> void try {
    if (!owner->current()) {
      finish(std::unexpected(
          AuthenticationFailure{AuthenticationFailureCode::RETIRED, {}}));
      return;
    }
    // Read ONLY required identifiers/type/source. Do not copy AuthContext's
    // username/token_hash or retain it in an output value.
    const auto& attributes = request->attributes();
    const auto& type = attributes->get<std::string>(auth::ATTR_AUTH_TYPE);
    const auto& source =
        attributes->get<std::string>(auth::ATTR_CREDENTIAL_SOURCE);
    const auto& user = attributes->get<std::string>(auth::ATTR_USER_ID);
    const auto& pat = attributes->get<std::string>(auth::ATTR_PAT_ID);
    if (type != "pat" || source != "bearer") {
      deny(response(drogon::k401Unauthorized, "pat_required",
                    "Bearer personal access token required", false));
      return;
    }
    if (!valid_uuid(user) || !valid_uuid(pat)) {
      deny(response(drogon::k401Unauthorized, "not_authenticated",
                    "Invalid authentication identity", false));
      return;
    }
    accept({Principal::Kind::PAT, user, pat});
  } catch (...) {
    unavailable();
  }

  auto start(std::optional<auth::test_seam::TokenValidator> validator)
      -> void try {
    const auto self = shared_from_this();
    if (!owner->current()) {
      finish(std::unexpected(
          AuthenticationFailure{AuthenticationFailureCode::RETIRED, {}}));
    } else if (mode == AuthenticationMode::PUBLIC) {
      if (safe_method(request->method()))
        accept({});
      else {
        origin_filter = std::make_shared<auth::PublicOriginFilter>();
        origin_filter->doFilter(
            request,
            [self](const drogon::HttpResponsePtr& rejected) {
              self->deny(rejected);
            },
            [self] { self->accept({}); });
      }
    } else if (validator) {
      auth::test_seam::dispatch_session_filter(
          request,
          [self](const drogon::HttpResponsePtr& rejected) {
            self->deny(rejected);
          },
          [self] { self->protected_success(); }, std::move(*validator));
    } else {
      session_filter = std::make_shared<auth::SessionFilter>();
      session_filter->doFilter(
          request,
          [self](const drogon::HttpResponsePtr& rejected) {
            self->deny(rejected);
          },
          [self] { self->protected_success(); });
    }
  } catch (...) {
    unavailable();
  }

  static auto begin(const drogon::HttpRequestPtr& request,
                    AuthenticationMode mode,
                    std::shared_ptr<AuthenticationOwner> owner,
                    AuthenticationCompletion completion,
                    std::optional<auth::test_seam::TokenValidator> validator)
      -> bool try {
    if (!request || !owner || !completion ||
        (mode != AuthenticationMode::PUBLIC &&
         mode != AuthenticationMode::PROTECTED))
      return false;
    std::size_t header_bytes = 0;
    const bool live = owner->current();
    constexpr auto MAX_CREDENTIAL =
        (EXTRA_BYTES_PER_LEASE - RUN_BYTES) / CREDENTIAL_COPY_FACTOR;
    const bool public_origin =
        mode == AuthenticationMode::PUBLIC && !safe_method(request->method());
    if (live && (mode == AuthenticationMode::PROTECTED || public_origin)) {
      // PublicOriginFilter also reads Cookie and copies Host/Origin during its
      // existing origin comparison. Reserve these copies without interpreting
      // or exporting any ambient public-route credential.
      constexpr std::array<std::string_view, 4> NAMES = {
          "authorization", "cookie", "host", "origin"};
      for (const auto name : NAMES) {
        if (!public_origin && name != "authorization" && name != "cookie")
          continue;
        const auto& value = request->getHeader(std::string{name});
        if (value.size() > MAX_CREDENTIAL - header_bytes) return false;
        header_bytes += value.size();
      }
    }
    // Reserve BEFORE a cookie accessor can materialize its transport parse.
    auto charge =
        owner->reserve(RUN_BYTES + CREDENTIAL_COPY_FACTOR * header_bytes);
    if (!charge) return false;
    std::optional<ExtraMemoryCharge> cookie_charge;
    if (live && (mode == AuthenticationMode::PROTECTED || public_origin)) {
      const auto& cookie = request->getCookie("plinth_session");
      const auto& csrf_cookie = request->getCookie("plinth_csrf");
      // addCookie() fixtures/cached transports may have a cookie map without a
      // raw Cookie header. Retention was admitted; charge any extra copies now.
      if (cookie.size() > MAX_CREDENTIAL ||
          csrf_cookie.size() > MAX_CREDENTIAL - cookie.size())
        return false;
      const auto cookie_bytes = cookie.size() + csrf_cookie.size();
      if (cookie_bytes > header_bytes) {
        const auto difference = cookie_bytes - header_bytes;
        if (difference > MAX_CREDENTIAL) return false;
        cookie_charge = owner->reserve(CREDENTIAL_COPY_FACTOR * difference);
        if (!cookie_charge) return false;
      }
    }
    auto run = std::make_shared<AuthenticationRun>(
        request, std::move(owner), std::move(completion), std::move(*charge),
        std::move(cookie_charge), mode);
    run->start(std::move(validator));
    return true;
  } catch (...) {
    return false;
  }
};

auto authenticate_http(const drogon::HttpRequestPtr& request,
                       AuthenticationMode mode,
                       std::shared_ptr<AuthenticationOwner> owner,
                       AuthenticationCompletion completion) -> bool {
  return AuthenticationRun::begin(request, mode, std::move(owner),
                                  std::move(completion), std::nullopt);
}
auto test_seam::authenticate_http(const drogon::HttpRequestPtr& request,
                                  AuthenticationMode mode,
                                  std::shared_ptr<AuthenticationOwner> owner,
                                  AuthenticationCompletion completion,
                                  auth::test_seam::TokenValidator validator)
    -> bool {
  return AuthenticationRun::begin(request, mode, std::move(owner),
                                  std::move(completion), std::move(validator));
}
} // namespace plinth::extension_http
