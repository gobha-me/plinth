#include "kernel/auth/crypto.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <argon2.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace plinth::auth {

namespace {

constexpr uint32_t ARGON2_TIME_COST = 4;
constexpr uint32_t ARGON2_MEMORY_KIB = 65536; // 64 MiB
constexpr uint32_t ARGON2_PARALLELISM = 4;
constexpr uint32_t ARGON2_HASH_LEN = 32;
constexpr uint32_t ARGON2_SALT_LEN = 16;

constexpr size_t TOKEN_BYTES = 32; // 256-bit

// Base64url alphabet (RFC 4648 section 5)
constexpr std::string_view BASE64URL_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

template <size_t N>
auto base64url_encode(const std::array<uint8_t, N>& data) -> std::string {
  std::string out;
  out.reserve(4 * ((N + 2) / 3));

  for (size_t i = 0; i < N; i += 3) {
    auto b0 = data.at(i);
    auto b1 = (i + 1 < N) ? data.at(i + 1) : uint8_t{0};
    auto b2 = (i + 2 < N) ? data.at(i + 2) : uint8_t{0};

    out.push_back(BASE64URL_ALPHABET[b0 >> 2]);
    out.push_back(BASE64URL_ALPHABET[((b0 & 0x03) << 4) | (b1 >> 4)]);
    if (i + 1 < N) {
      out.push_back(BASE64URL_ALPHABET[((b1 & 0x0F) << 2) | (b2 >> 6)]);
    }
    if (i + 2 < N) {
      out.push_back(BASE64URL_ALPHABET[b2 & 0x3F]);
    }
  }

  return out;
}

auto generate_salt() -> std::array<uint8_t, ARGON2_SALT_LEN> {
  std::array<uint8_t, ARGON2_SALT_LEN> salt{};
  if (RAND_bytes(salt.data(), ARGON2_SALT_LEN) != 1) {
    throw std::runtime_error("CSPRNG failure: RAND_bytes failed");
  }
  return salt;
}

auto is_valid_username_char(char ch) -> bool {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
         ch == '-';
}

} // namespace

auto hash_password(const std::string& password) -> std::string {
  auto salt = generate_salt();

  // argon2id_hash_encoded writes into a caller-supplied buffer
  // Max encoded length for these parameters is ~128 bytes
  std::vector<char> encoded(256);

  auto rc = argon2id_hash_encoded(
      ARGON2_TIME_COST, ARGON2_MEMORY_KIB, ARGON2_PARALLELISM, password.data(),
      password.size(), salt.data(), salt.size(), ARGON2_HASH_LEN,
      encoded.data(), encoded.size());

  if (rc != ARGON2_OK) {
    throw std::runtime_error(std::string{"argon2id_hash_encoded failed: "} +
                             argon2_error_message(rc));
  }

  return std::string{encoded.data()};
}

auto verify_password(const std::string& password, const std::string& hash)
    -> bool {
  auto rc = argon2id_verify(hash.c_str(), password.data(), password.size());

  return rc == ARGON2_OK;
}

auto dummy_hash() -> void {
  // Run a real argon2id hash to consume the same time as a real verification.
  // The result is discarded.
  static const std::string DUMMY_PASSWORD = "dummy-timing-equalization";
  static const std::string DUMMY_HASH_STR = hash_password(DUMMY_PASSWORD);
  // timing equalization
  argon2id_verify(DUMMY_HASH_STR.c_str(), DUMMY_PASSWORD.data(),
                  DUMMY_PASSWORD.size());
}

auto generate_token() -> std::string {
  std::array<uint8_t, TOKEN_BYTES> bytes{};
  if (RAND_bytes(bytes.data(), TOKEN_BYTES) != 1) {
    throw std::runtime_error("CSPRNG failure: RAND_bytes failed");
  }
  return base64url_encode(bytes);
}

auto sha256_hex(const std::string& input) -> std::string {
  auto* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    throw std::runtime_error("EVP_MD_CTX_new failed");
  }

  std::array<uint8_t, 32> hash{};
  unsigned int hash_len = 0;

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, input.data(), input.size()) != 1 ||
      EVP_DigestFinal_ex(ctx, hash.data(), &hash_len) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("SHA-256 computation failed");
  }
  EVP_MD_CTX_free(ctx);

  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < hash_len; ++i) {
    ss << std::setw(2) << static_cast<int>(hash.at(i));
  }
  return ss.str();
}

auto validate_username(const std::string& username)
    -> std::optional<std::string> {
  if (username.size() < 3) {
    return "username_too_short";
  }
  if (username.size() > 64) {
    return "username_too_long";
  }
  for (char ch : username) {
    if (!is_valid_username_char(ch)) {
      return "username_invalid_chars";
    }
  }
  return std::nullopt;
}

auto validate_password(const std::string& password)
    -> std::optional<std::string> {
  if (password.size() < 8) {
    return "password_too_short";
  }
  return std::nullopt;
}

} // namespace plinth::auth
