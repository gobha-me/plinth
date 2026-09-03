// SPDX-License-Identifier: MIT
//
// `crypto.*` host bindings — ICD-0.3.2 §Injected Surface / crypto.* +
// §Security Constraints 3–4.
//
// OpenSSL-backed: EVP_Digest for `hash`, RAND_bytes for `randomBytes`.
// `timingSafeEqual` is an XOR-accumulator over equal-length bytes
// (length mismatch short-circuits false; matches Node's convention).

#include "kernel/js/stdlib_inject.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <quickjs.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::js {

namespace {

// Accepted algorithms — enforced; OpenSSL exposing more is irrelevant.
// Adding a value means updating both this table and the ICD.
constexpr std::string_view ALG_SHA256 = "sha256";
constexpr std::string_view ALG_SHA512 = "sha512";

// randomBytes length bounds — ICD-0.3.2 §Security Constraint 3.
constexpr int RANDOM_BYTES_MIN = 1;
constexpr int RANDOM_BYTES_MAX = 4096;

// Reads bytes from an `args[i]` that may be either a string (UTF-8
// byte sequence) or a Uint8Array (raw buffer view). Appends into
// `out`. Returns false with an exception already thrown on mismatch.
auto read_bytes_arg(JSContext* ctx, JSValue v, std::vector<uint8_t>& out)
    -> bool {
  if (JS_IsString(v)) {
    std::size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (s == nullptr) {
      return false;
    }
    const auto* start = reinterpret_cast<const uint8_t*>(s);
    out.assign(std::span<const uint8_t>{start, len}.begin(),
               std::span<const uint8_t>{start, len}.end());
    JS_FreeCString(ctx, s);
    return true;
  }
  if (JS_GetTypedArrayType(v) == JS_TYPED_ARRAY_UINT8) {
    std::size_t byte_offset = 0;
    std::size_t byte_length = 0;
    std::size_t bytes_per_elem = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &byte_offset, &byte_length,
                                         &bytes_per_elem);
    if (JS_IsException(buf)) {
      return false;
    }
    std::size_t total = 0;
    uint8_t* data = JS_GetArrayBuffer(ctx, &total, buf);
    if (data == nullptr) {
      JS_FreeValue(ctx, buf);
      return false;
    }
    std::span<const uint8_t> view(data, total);
    auto slice = view.subspan(byte_offset, byte_length);
    out.assign(slice.begin(), slice.end());
    JS_FreeValue(ctx, buf);
    return true;
  }
  JS_ThrowTypeError(ctx, "crypto: expected string or Uint8Array");
  return false;
}

// Wrap the raw QuickJS argv pointer once; callers index through the span.
auto make_arg_span(int argc, JSValue* argv) -> std::span<const JSValue> {
  return {argv, static_cast<std::size_t>(argc)};
}

auto bytes_to_hex(std::span<const unsigned char> bytes) -> std::string {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (auto byte : bytes) {
    ss << std::setw(2) << static_cast<int>(byte);
  }
  return ss.str();
}

auto crypto_hash(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  auto args = make_arg_span(argc, argv);
  if (args.empty() || !JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "crypto.hash: expected string at arg 0");
  }
  if (args.size() < 2) {
    return JS_ThrowTypeError(
        ctx, "crypto.hash: expected string or Uint8Array at arg 1");
  }

  std::size_t alg_len = 0;
  const char* alg_cstr = JS_ToCStringLen(ctx, &alg_len, args[0]);
  if (alg_cstr == nullptr) {
    return JS_EXCEPTION;
  }
  std::string_view alg{alg_cstr, alg_len};

  const EVP_MD* md = nullptr;
  if (alg == ALG_SHA256) {
    md = EVP_sha256();
  } else if (alg == ALG_SHA512) {
    md = EVP_sha512();
  } else {
    JSValue err = JS_ThrowRangeError(
        ctx, "crypto.hash: unsupported algorithm: %s", alg_cstr);
    JS_FreeCString(ctx, alg_cstr);
    return err;
  }
  JS_FreeCString(ctx, alg_cstr);

  std::vector<uint8_t> data;
  if (!read_bytes_arg(ctx, args[1], data)) {
    return JS_EXCEPTION;
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
  if (mdctx == nullptr) {
    return JS_ThrowInternalError(ctx, "crypto.hash: EVP_MD_CTX_new failed");
  }
  if (EVP_DigestInit_ex(mdctx, md, nullptr) != 1 ||
      EVP_DigestUpdate(mdctx, data.data(), data.size()) != 1 ||
      EVP_DigestFinal_ex(mdctx, digest.data(), &digest_len) != 1) {
    EVP_MD_CTX_free(mdctx);
    return JS_ThrowInternalError(ctx, "crypto.hash: digest failed");
  }
  EVP_MD_CTX_free(mdctx);

  std::string hex = bytes_to_hex(std::span{digest}.first(digest_len));
  return JS_NewStringLen(ctx, hex.data(), hex.size());
}

auto crypto_random_bytes(JSContext* ctx, JSValue /*this_val*/, int argc,
                         JSValue* argv) -> JSValue {
  auto args = make_arg_span(argc, argv);
  if (args.empty() || !JS_IsNumber(args[0])) {
    return JS_ThrowTypeError(ctx,
                             "crypto.randomBytes: expected integer at arg 0");
  }
  int32_t n = 0;
  if (JS_ToInt32(ctx, &n, args[0]) != 0) {
    return JS_EXCEPTION;
  }
  if (n < RANDOM_BYTES_MIN || n > RANDOM_BYTES_MAX) {
    return JS_ThrowRangeError(ctx,
                              "crypto.randomBytes: n out of range [%d, %d]",
                              RANDOM_BYTES_MIN, RANDOM_BYTES_MAX);
  }
  std::vector<uint8_t> buf(static_cast<std::size_t>(n));
  if (RAND_bytes(buf.data(), n) != 1) {
    return JS_ThrowInternalError(ctx, "crypto.randomBytes: RNG failure");
  }
  return JS_NewUint8ArrayCopy(ctx, buf.data(), buf.size());
}

auto crypto_timing_safe_equal(JSContext* ctx, JSValue /*this_val*/, int argc,
                              JSValue* argv) -> JSValue {
  auto args = make_arg_span(argc, argv);
  if (args.size() < 2) {
    return JS_ThrowTypeError(
        ctx, "crypto.timingSafeEqual: expected two Uint8Arrays");
  }
  if (JS_GetTypedArrayType(args[0]) != JS_TYPED_ARRAY_UINT8 ||
      JS_GetTypedArrayType(args[1]) != JS_TYPED_ARRAY_UINT8) {
    return JS_ThrowTypeError(
        ctx, "crypto.timingSafeEqual: expected Uint8Array args");
  }
  std::vector<uint8_t> a;
  std::vector<uint8_t> b;
  if (!read_bytes_arg(ctx, args[0], a)) {
    return JS_EXCEPTION;
  }
  if (!read_bytes_arg(ctx, args[1], b)) {
    return JS_EXCEPTION;
  }
  if (a.size() != b.size()) {
    return JS_NewBool(ctx, false);
  }
  uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return JS_NewBool(ctx, diff == 0);
}

} // namespace

auto register_crypto(JSContext* ctx) -> void {
  inject_sync_fn(ctx, "crypto", "hash", &crypto_hash, 2);
  inject_sync_fn(ctx, "crypto", "randomBytes", &crypto_random_bytes, 1);
  inject_sync_fn(ctx, "crypto", "timingSafeEqual", &crypto_timing_safe_equal,
                 2);
}

} // namespace plinth::js
