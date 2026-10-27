#include "kernel/packages/http_install_authorization.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/manifest.hpp"
#include "kernel/rbac/enforcement.hpp"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace plinth::packages {
namespace {

auto digest(std::string_view text) -> std::string {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
  if (SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(),
             bytes.data()) == nullptr)
    throw std::runtime_error("HTTP submission digest unavailable");
  constexpr std::string_view HEX = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(HEX[byte >> 4]);
    result.push_back(HEX[byte & 15]);
  }
  return result;
}

auto valid_uuid(std::string_view value) -> bool {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!((value[index] >= '0' && value[index] <= '9') ||
                 (value[index] >= 'a' && value[index] <= 'f')))
      return false;
  }
  return true;
}

auto reject(HttpInstallAuthorizationCode code, std::string message)
    -> HttpInstallAuthorizationResult {
  return std::unexpected(
      HttpInstallAuthorizationError{code, std::move(message)});
}

auto valid_submission(const HttpInstallAuthorizationRequest& request) -> bool {
  if (request.operation == HttpInstallOperation::INSTALL) {
    if (!request.predecessor_id.empty()) return false;
  } else if (request.operation == HttpInstallOperation::UPGRADE) {
    if (!valid_uuid(request.predecessor_id)) return false;
  } else
    return false;
  if (request.provenance != Provenance::USER &&
      request.provenance != Provenance::BUNDLED)
    return false;
  const auto typed = HttpDeclaration::parse(request.raw_manifest);
  if (!typed.value || *typed.value != request.declaration) return false;
  try {
    const auto manifest =
        PackageManifest::parse(request.raw_manifest, "manifest.json",
                               request.provenance == Provenance::BUNDLED);
    return manifest.value && manifest.value->name == request.package_name &&
           manifest.value->version == request.package_version;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

} // namespace

auto http_install_zip_sha256(std::span<const std::byte> bytes) -> std::string {
  return digest({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

auto http_install_declarations_sha256(const HttpDeclaration& declaration)
    -> std::string {
  nlohmann::ordered_json document;
  document["schema"] = 1;
  document["http_prefixes"] = declaration.http_prefixes;
  document["unauthenticated_prefixes"] = declaration.unauthenticated_prefixes;
  document["handler_mode"] = nullptr;
  if (declaration.handler_mode) {
    switch (*declaration.handler_mode) {
      case HttpHandlerMode::QUICKJS:
        document["handler_mode"] = "quickjs";
        break;
      case HttpHandlerMode::SIDECAR:
        document["handler_mode"] = "sidecar";
        break;
      case HttpHandlerMode::BUNDLED_NATIVE:
        document["handler_mode"] = "bundled_native";
        break;
    }
  }
  std::string transcript{"plinth.http.declarations.v1"};
  transcript.push_back('\0');
  transcript += document.dump();
  return digest(transcript);
}

auto http_public_confirmation_sha256(std::string_view zip_sha256,
                                     std::string_view declarations_sha256,
                                     std::string_view actor_user_id)
    -> std::string {
  std::string transcript{"plinth.http.install.confirm.v1"};
  for (const auto field : {zip_sha256, declarations_sha256, actor_user_id}) {
    transcript.push_back('\0');
    transcript.append(field);
  }
  return digest(transcript);
}

AuthorizedHttpSubmission::AuthorizedHttpSubmission(
    std::string actor, const HttpInstallAuthorizationRequest& request,
    std::string zip_digest, std::string manifest_digest,
    std::string declaration_digest, bool bundled)
    : actor(std::move(actor)), name(request.package_name),
      version(request.package_version), zip_digest(std::move(zip_digest)),
      manifest_digest(std::move(manifest_digest)),
      declaration_digest(std::move(declaration_digest)), op(request.operation),
      predecessor(request.predecessor_id), origin(request.provenance),
      declarations(request.declaration), bundled(bundled) {
}

auto AuthorizedHttpSubmission::matches(
    const HttpInstallAuthorizationRequest& request) const -> bool {
  return name == request.package_name && version == request.package_version &&
         op == request.operation && predecessor == request.predecessor_id &&
         origin == request.provenance && declarations == request.declaration &&
         zip_digest == http_install_zip_sha256(request.zip_blob) &&
         manifest_digest == digest(request.raw_manifest) &&
         declaration_digest ==
             http_install_declarations_sha256(request.declaration);
}

class HttpInstallAuthorizationFactory {
 public:
  static auto request(std::string actor, std::vector<std::string> rules,
                      bool granted, std::string confirmation,
                      std::string expected_zip) -> HttpInstallAuthorizer {
    return [actor = std::move(actor), rules = std::move(rules), granted,
            confirmation = std::move(confirmation),
            expected_zip = std::move(expected_zip)](
               const HttpInstallAuthorizationRequest& input)
               -> HttpInstallAuthorizationResult {
      if (!granted || !valid_uuid(actor) ||
          input.provenance != Provenance::USER) {
        return reject(HttpInstallAuthorizationCode::PERMISSION_DENIED,
                      "HTTP package submission requires authenticated "
                      "installer authority");
      }
      const auto has = [&](std::string_view rule) {
        return std::ranges::find(rules, "kernel.admin") != rules.end() ||
               std::ranges::find(rules, rule) != rules.end();
      };
      if (!has("packages.install") ||
          (!input.declaration.http_prefixes.empty() &&
           !has("packages.install.with_http_prefixes")) ||
          (!input.declaration.unauthenticated_prefixes.empty() &&
           !has("packages.install.with_unauthenticated_prefixes"))) {
        return reject(
            HttpInstallAuthorizationCode::PERMISSION_DENIED,
            "HTTP package submission lacks required installer permissions");
      }
      if (!valid_submission(input) ||
          expected_zip != http_install_zip_sha256(input.zip_blob)) {
        return reject(HttpInstallAuthorizationCode::INVALID_SUBMISSION,
                      "HTTP authorization does not match the submitted "
                      "artifact and declarations");
      }
      const auto declarations =
          http_install_declarations_sha256(input.declaration);
      if (!input.declaration.unauthenticated_prefixes.empty() &&
          confirmation != http_public_confirmation_sha256(
                              expected_zip, declarations, actor)) {
        return reject(HttpInstallAuthorizationCode::CONFIRMATION_REQUIRED,
                      "Public HTTP claims require artifact, declaration and "
                      "actor confirmation");
      }
      return AuthorizedHttpSubmission{actor,        input,
                                      expected_zip, digest(input.raw_manifest),
                                      declarations, false};
    };
  }

  static auto bundle(std::string zip_digest, HttpInstallOperation operation,
                     std::string predecessor) -> HttpInstallAuthorizer {
    return [zip_digest = std::move(zip_digest), operation,
            predecessor = std::move(predecessor)](
               const HttpInstallAuthorizationRequest& input)
               -> HttpInstallAuthorizationResult {
      if (input.provenance != Provenance::BUNDLED ||
          input.package_name != "shell" || input.operation != operation ||
          input.predecessor_id != predecessor ||
          http_install_zip_sha256(input.zip_blob) != zip_digest ||
          !valid_submission(input)) {
        return reject(HttpInstallAuthorizationCode::PERMISSION_DENIED,
                      "HTTP bundle authorization does not match "
                      "kernel-selected Shell provenance");
      }
      const auto manifest =
          PackageManifest::parse(input.raw_manifest, "manifest.json", true);
      if (!manifest.value || !manifest.value->frontend) {
        return reject(
            HttpInstallAuthorizationCode::INVALID_SUBMISSION,
            "Kernel-selected Shell requires its validated frontend manifest");
      }
      return AuthorizedHttpSubmission{
          "",
          input,
          zip_digest,
          digest(input.raw_manifest),
          http_install_declarations_sha256(input.declaration),
          true};
    };
  }
};

auto make_http_install_authorizer(const drogon::HttpRequestPtr& request,
                                  std::span<const std::byte> zip_blob)
    -> HttpInstallAuthorizer {
  if (!request) {
    return HttpInstallAuthorizationFactory::request(
        {}, {}, false, {}, http_install_zip_sha256(zip_blob));
  }
  const auto actor = auth::get_auth_context(request);
  const auto rules = rbac::get_rbac_context(request);
  const bool authenticated =
      actor && (actor->auth_type == "session" || actor->auth_type == "pat");
  return HttpInstallAuthorizationFactory::request(
      authenticated ? actor->user_id : "",
      rules ? rules->effective_rules : std::vector<std::string>{},
      authenticated && rules && rules->permission_granted,
      request->getHeader("x-plinth-confirm-public-http"),
      http_install_zip_sha256(zip_blob));
}

KernelBundledHttpInstallToken::KernelBundledHttpInstallToken(
    std::span<const std::byte> bytes, HttpInstallOperation operation,
    std::string_view predecessor_id)
    : zip_digest(http_install_zip_sha256(bytes)), operation(operation),
      predecessor(predecessor_id) {
}

auto make_bundled_http_install_authorizer(KernelBundledHttpInstallToken token)
    -> HttpInstallAuthorizer {
  return HttpInstallAuthorizationFactory::bundle(std::move(token.zip_digest),
                                                 token.operation,
                                                 std::move(token.predecessor));
}

} // namespace plinth::packages
