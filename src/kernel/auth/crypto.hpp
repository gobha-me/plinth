#pragma once

#include <optional>
#include <string>

namespace plinth::auth {

// Hash a password with argon2id (memory=64MiB, iterations=4, parallelism=4).
// Returns the encoded hash string (starts with "$argon2id$").
auto hash_password(const std::string& password) -> std::string;

// Verify a password against an argon2id hash. Constant-time.
auto verify_password(const std::string& password, const std::string& hash)
    -> bool;

// Run a dummy argon2id hash to prevent timing side-channels when a username
// is not found (so that missing-user and wrong-password take the same time).
auto dummy_hash() -> void;

// Generate a 256-bit CSPRNG token, base64url-encoded (43 characters, no
// padding).
auto generate_token() -> std::string;

// Compute SHA-256 of input, return as lowercase hex string.
auto sha256_hex(const std::string& input) -> std::string;

// Validate a username per ICD-0.1.2 rules: 3-64 chars, [a-z0-9_-] only.
// Returns the error code string if invalid, or nullopt if valid.
auto validate_username(const std::string& username)
    -> std::optional<std::string>;

// Validate password: minimum 8 characters.
// Returns the error code string if invalid, or nullopt if valid.
auto validate_password(const std::string& password)
    -> std::optional<std::string>;

} // namespace plinth::auth
