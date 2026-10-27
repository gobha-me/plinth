#pragma once

#include "kernel/packages/http_declarations.hpp"
#include "kernel/shell/firstboot.hpp"

#include <drogon/HttpRequest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace plinth::packages {

enum class Provenance : std::uint8_t;
enum class HttpInstallOperation : std::uint8_t { INSTALL, UPGRADE };
enum class HttpInstallAuthorizationCode : std::uint8_t {
  PERMISSION_DENIED,
  CONFIRMATION_REQUIRED,
  INVALID_SUBMISSION,
};

struct HttpInstallAuthorizationError {
  HttpInstallAuthorizationCode code;
  std::string message;
};

// Borrowed only for the synchronous producer call. The producer verifies the
// typed declaration and package identity against the ORIGINAL manifest bytes.
struct HttpInstallAuthorizationRequest {
  std::span<const std::byte> zip_blob;
  std::string_view raw_manifest;
  const HttpDeclaration& declaration;
  std::string_view package_name;
  std::string_view package_version;
  HttpInstallOperation operation;
  std::string_view predecessor_id;
  Provenance provenance;
};

class HttpInstallAuthorizationFactory;

// Kernel-produced operation proof, not a receipt, reservation or readiness
// grant. No public constructor or actor/permission boolean can mint one.
class AuthorizedHttpSubmission {
 public:
  AuthorizedHttpSubmission(AuthorizedHttpSubmission&&) noexcept = default;
  auto operator=(AuthorizedHttpSubmission&&) noexcept
      -> AuthorizedHttpSubmission& = default;
  AuthorizedHttpSubmission(const AuthorizedHttpSubmission&) = delete;
  auto operator=(const AuthorizedHttpSubmission&)
      -> AuthorizedHttpSubmission& = delete;

  [[nodiscard]] auto actor_user_id() const -> const std::string& {
    return actor;
  }
  [[nodiscard]] auto package_name() const -> const std::string& { return name; }
  [[nodiscard]] auto package_version() const -> const std::string& {
    return version;
  }
  [[nodiscard]] auto zip_sha256() const -> const std::string& {
    return zip_digest;
  }
  [[nodiscard]] auto raw_manifest_sha256() const -> const std::string& {
    return manifest_digest;
  }
  [[nodiscard]] auto declarations_sha256() const -> const std::string& {
    return declaration_digest;
  }
  [[nodiscard]] auto operation() const -> HttpInstallOperation { return op; }
  [[nodiscard]] auto predecessor_id() const -> const std::string& {
    return predecessor;
  }
  [[nodiscard]] auto provenance() const -> Provenance { return origin; }
  [[nodiscard]] auto declaration() const -> const HttpDeclaration& {
    return declarations;
  }
  [[nodiscard]] auto kernel_selected_bundle() const -> bool { return bundled; }
  [[nodiscard]] auto matches(
      const HttpInstallAuthorizationRequest& request) const -> bool;

 private:
  friend class HttpInstallAuthorizationFactory;
  AuthorizedHttpSubmission(std::string actor,
                           const HttpInstallAuthorizationRequest& request,
                           std::string zip_digest, std::string manifest_digest,
                           std::string declaration_digest, bool bundled);
  std::string actor;
  std::string name;
  std::string version;
  std::string zip_digest;
  std::string manifest_digest;
  std::string declaration_digest;
  HttpInstallOperation op;
  std::string predecessor;
  Provenance origin;
  HttpDeclaration declarations;
  bool bundled;
};

using HttpInstallAuthorizationResult =
    std::expected<AuthorizedHttpSubmission, HttpInstallAuthorizationError>;
using HttpInstallAuthorizer = std::function<HttpInstallAuthorizationResult(
    const HttpInstallAuthorizationRequest&)>;

// Captures owned identity/rules/confirmation snapshots from the actual request
// after SessionFilter/RbacFilter. No raw credential or request is retained.
[[nodiscard]] auto make_http_install_authorizer(
    const drogon::HttpRequestPtr& request, std::span<const std::byte> zip_blob)
    -> HttpInstallAuthorizer;

// Public digest material is acknowledgement data, never authorization.
[[nodiscard]] auto http_install_zip_sha256(std::span<const std::byte> bytes)
    -> std::string;
[[nodiscard]] auto http_install_declarations_sha256(
    const HttpDeclaration& declaration) -> std::string;
[[nodiscard]] auto http_public_confirmation_sha256(
    std::string_view zip_sha256, std::string_view declarations_sha256,
    std::string_view actor_user_id) -> std::string;

// Only the actual kernel-selected firstboot operation can create this token.
// USER, an empty actor or a manifest provenance field cannot construct it.
class KernelBundledHttpInstallToken {
 public:
  KernelBundledHttpInstallToken(KernelBundledHttpInstallToken&&) noexcept =
      default;
  KernelBundledHttpInstallToken(const KernelBundledHttpInstallToken&) = delete;
  auto operator=(const KernelBundledHttpInstallToken&)
      -> KernelBundledHttpInstallToken& = delete;

 private:
  friend auto plinth::shell::ensure_bundled_shell_installed(
      const Config&, const InstallerContext&, bool)
      -> std::expected<void, plinth::shell::FirstBootFailure>;
  friend auto make_bundled_http_install_authorizer(
      KernelBundledHttpInstallToken token) -> HttpInstallAuthorizer;
  KernelBundledHttpInstallToken(std::span<const std::byte> bytes,
                                HttpInstallOperation operation,
                                std::string_view predecessor_id);
  std::string zip_digest;
  HttpInstallOperation operation;
  std::string predecessor;
};

[[nodiscard]] auto make_bundled_http_install_authorizer(
    KernelBundledHttpInstallToken token) -> HttpInstallAuthorizer;

} // namespace plinth::packages
