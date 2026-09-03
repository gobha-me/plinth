#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

#include "kernel/auth/crypto.hpp"

// ── Password hashing ─────────────────────────────────────────

TEST_CASE("hash_password returns argon2id encoded string",
          "[auth][crypto][unit]") {
  auto hash = plinth::auth::hash_password("test-password-123");
  REQUIRE(hash.substr(0, 10) == "$argon2id$");
  REQUIRE(hash.size() > 50);
}

TEST_CASE("verify_password succeeds for correct password",
          "[auth][crypto][unit]") {
  auto hash = plinth::auth::hash_password("correct-horse-battery-staple");
  REQUIRE(plinth::auth::verify_password("correct-horse-battery-staple", hash));
}

TEST_CASE("verify_password fails for wrong password", "[auth][crypto][unit]") {
  auto hash = plinth::auth::hash_password("correct-horse-battery-staple");
  REQUIRE_FALSE(plinth::auth::verify_password("wrong-password", hash));
}

TEST_CASE(
    "hash_password produces different hashes for same password (unique salts)",
    "[auth][crypto][unit]") {
  auto h1 = plinth::auth::hash_password("same-password");
  auto h2 = plinth::auth::hash_password("same-password");
  REQUIRE(h1 != h2);
}

TEST_CASE("dummy_hash does not throw", "[auth][crypto][unit]") {
  REQUIRE_NOTHROW(plinth::auth::dummy_hash());
}

// ── Token generation ─────────────────────────────────────────

TEST_CASE("generate_token returns 43-char base64url string",
          "[auth][crypto][unit]") {
  auto token = plinth::auth::generate_token();
  REQUIRE(token.size() == 43);

  // Verify all characters are valid base64url
  for (char ch : token) {
    bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    REQUIRE(valid);
  }
}

TEST_CASE("generate_token produces unique tokens", "[auth][crypto][unit]") {
  std::set<std::string> tokens;
  for (int i = 0; i < 100; ++i) {
    tokens.insert(plinth::auth::generate_token());
  }
  REQUIRE(tokens.size() == 100);
}

// ── SHA-256 ──────────────────────────────────────────────────

TEST_CASE("sha256_hex produces correct hash for known input",
          "[auth][crypto][unit]") {
  // SHA-256("hello") = well-known value
  auto hex = plinth::auth::sha256_hex("hello");
  REQUIRE(hex ==
          "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST_CASE("sha256_hex produces 64-char lowercase hex", "[auth][crypto][unit]") {
  auto hex = plinth::auth::sha256_hex("arbitrary input");
  REQUIRE(hex.size() == 64);
  for (char ch : hex) {
    bool valid = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    REQUIRE(valid);
  }
}

TEST_CASE("sha256_hex handles empty string", "[auth][crypto][unit]") {
  // SHA-256("") = well-known value
  auto hex = plinth::auth::sha256_hex("");
  REQUIRE(hex ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ── Username validation ──────────────────────────────────────

TEST_CASE("validate_username accepts valid names", "[auth][crypto][unit]") {
  REQUIRE_FALSE(plinth::auth::validate_username("alice").has_value());
  REQUIRE_FALSE(plinth::auth::validate_username("bob-123").has_value());
  REQUIRE_FALSE(plinth::auth::validate_username("test_user").has_value());
  REQUIRE_FALSE(plinth::auth::validate_username("abc").has_value());
  // 64 chars (max)
  REQUIRE_FALSE(
      plinth::auth::validate_username(std::string(64, 'a')).has_value());
}

TEST_CASE("validate_username rejects too short", "[auth][crypto][unit]") {
  auto err = plinth::auth::validate_username("ab");
  REQUIRE(err.has_value());
  REQUIRE(err.value() == "username_too_short");
}

TEST_CASE("validate_username rejects too long", "[auth][crypto][unit]") {
  auto err = plinth::auth::validate_username(std::string(65, 'a'));
  REQUIRE(err.has_value());
  REQUIRE(err.value() == "username_too_long");
}

TEST_CASE("validate_username rejects uppercase", "[auth][crypto][unit]") {
  auto err = plinth::auth::validate_username("Alice");
  REQUIRE(err.has_value());
  REQUIRE(err.value() == "username_invalid_chars");
}

TEST_CASE("validate_username rejects spaces and special chars",
          "[auth][crypto][unit]") {
  REQUIRE(plinth::auth::validate_username("has space").has_value());
  REQUIRE(plinth::auth::validate_username("has@sign").has_value());
  REQUIRE(plinth::auth::validate_username("has.dot").has_value());
}

// ── Password validation ──────────────────────────────────────

TEST_CASE("validate_password accepts 8+ chars", "[auth][crypto][unit]") {
  REQUIRE_FALSE(plinth::auth::validate_password("12345678").has_value());
  REQUIRE_FALSE(plinth::auth::validate_password("correct-horse-battery-staple")
                    .has_value());
}

TEST_CASE("validate_password rejects too short", "[auth][crypto][unit]") {
  auto err = plinth::auth::validate_password("1234567");
  REQUIRE(err.has_value());
  REQUIRE(err.value() == "password_too_short");
}
