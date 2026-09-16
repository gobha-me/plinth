#include "kernel/auth/csrf.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/browser_origin.hpp"

#include <array>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdexcept>

namespace plinth::auth {

namespace {

constexpr std::string_view CSRF_DOMAIN = "plinth.csrf.v1";
constexpr std::string_view BASE64URL_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::mutex origin_mutex;
std::string configured_origin;

auto current_browser_origin() -> std::string {
  const std::scoped_lock lock{origin_mutex};
  return configured_origin;
}

auto base64url_encode(const std::array<unsigned char, 32>& data)
    -> std::string {
  std::string output;
  output.reserve(43);
  for (std::size_t index = 0; index < data.size(); index += 3) {
    const auto first = data.at(index);
    const auto second = index + 1 < data.size() ? data.at(index + 1) : 0;
    const auto third = index + 2 < data.size() ? data.at(index + 2) : 0;
    output.push_back(BASE64URL_ALPHABET.at(first >> 2));
    output.push_back(
        BASE64URL_ALPHABET.at(((first & 0x03U) << 4U) | (second >> 4U)));
    if (index + 1 < data.size()) {
      output.push_back(
          BASE64URL_ALPHABET.at(((second & 0x0FU) << 2U) | (third >> 6U)));
    }
    if (index + 2 < data.size()) {
      output.push_back(BASE64URL_ALPHABET.at(third & 0x3FU));
    }
  }
  return output;
}

auto constant_time_equal(std::string_view left, std::string_view right)
    -> bool {
  return left.size() == right.size() &&
         CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

auto is_safe_method(drogon::HttpMethod method) -> bool {
  return method == drogon::Get || method == drogon::Head ||
         method == drogon::Options;
}

auto csrf_failure() -> drogon::HttpResponsePtr {
  Json::Value body;
  body["error"] = "csrf_failed";
  body["message"] = "Request validation failed";
  auto response = drogon::HttpResponse::newHttpJsonResponse(body);
  response->setStatusCode(drogon::k403Forbidden);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Vary", "Origin, Cookie, Authorization");
  return response;
}

} // namespace

auto csrf_token_for_session(std::string_view raw_session_token) -> std::string {
  auto* key = EVP_PKEY_new_raw_private_key(
      EVP_PKEY_HMAC, nullptr,
      reinterpret_cast<const unsigned char*>(raw_session_token.data()),
      raw_session_token.size());
  if (key == nullptr) {
    throw std::runtime_error("CSRF HMAC key initialization failed");
  }

  auto* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    EVP_PKEY_free(key);
    throw std::runtime_error("CSRF HMAC context initialization failed");
  }

  std::array<unsigned char, 32> digest{};
  std::size_t digest_size = digest.size();
  const bool ok =
      EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
      EVP_DigestSignUpdate(context, CSRF_DOMAIN.data(), CSRF_DOMAIN.size()) ==
          1 &&
      EVP_DigestSignFinal(context, digest.data(), &digest_size) == 1 &&
      digest_size == digest.size();
  EVP_MD_CTX_free(context);
  EVP_PKEY_free(key);
  if (!ok) {
    throw std::runtime_error("CSRF HMAC computation failed");
  }
  return base64url_encode(digest);
}

auto request_expected_csrf_token(const drogon::HttpRequestPtr& request)
    -> std::optional<std::string> {
  auto value = request->attributes()->get<std::string>(ATTR_CSRF_TOKEN);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

auto add_csrf_cookie(drogon::HttpResponsePtr& response,
                     const std::string& token, bool dev_mode) -> void {
  auto cookie = drogon::Cookie(std::string{CSRF_COOKIE}, token);
  cookie.setPath("/");
  cookie.setHttpOnly(false);
  cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
  if (!dev_mode) {
    cookie.setSecure(true);
  }
  cookie.setMaxAge(86400);
  response->addCookie(std::move(cookie));
}

auto clear_csrf_cookie(drogon::HttpResponsePtr& response) -> void {
  auto cookie = drogon::Cookie(std::string{CSRF_COOKIE}, "");
  cookie.setPath("/");
  cookie.setHttpOnly(false);
  cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
  cookie.setMaxAge(0);
  response->addCookie(std::move(cookie));
}

auto configure_browser_origin(std::string origin) -> void {
  const std::scoped_lock lock{origin_mutex};
  configured_origin = std::move(origin);
}

auto CsrfFilter::doFilter(const drogon::HttpRequestPtr& request,
                          drogon::FilterCallback&& filter_callback,
                          drogon::FilterChainCallback&& chain_callback)
    -> void {
  if (is_safe_method(request->method())) {
    chain_callback();
    return;
  }

  const auto context = get_auth_context(request);
  if (context.has_value() &&
      context->credential_source == CredentialSource::BEARER) {
    chain_callback();
    return;
  }
  if (!context.has_value() ||
      context->credential_source != CredentialSource::COOKIE) {
    filter_callback(csrf_failure());
    return;
  }

  const auto expected = request_expected_csrf_token(request);
  const auto cookie = request->getCookie(std::string{CSRF_COOKIE});
  const auto header = request->getHeader(std::string{CSRF_HEADER});
  if (!expected.has_value() || expected->size() != 43 ||
      !browser_origin_matches(request, current_browser_origin()) ||
      !constant_time_equal(cookie, *expected) ||
      !constant_time_equal(header, *expected)) {
    filter_callback(csrf_failure());
    return;
  }
  chain_callback();
}

auto PublicOriginFilter::doFilter(const drogon::HttpRequestPtr& request,
                                  drogon::FilterCallback&& filter_callback,
                                  drogon::FilterChainCallback&& chain_callback)
    -> void {
  const auto& origin = request->getHeader("origin");
  if ((!origin.empty() &&
       !browser_origin_matches(request, current_browser_origin())) ||
      (origin.empty() && browser_shaped_request(request))) {
    filter_callback(csrf_failure());
    return;
  }
  chain_callback();
}

} // namespace plinth::auth
