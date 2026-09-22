#pragma once

#include "kernel/config.hpp"

#include <string>

namespace plinth::auth {

namespace test_seam {

// Deterministic coverage for the process-wide Argon2 memory admission bound.
// Production code acquires the same slots before every password hash/verify.
auto try_acquire_password_hash_slot() -> bool;
auto release_password_hash_slot() -> void;
auto active_password_hash_slots() -> unsigned int;

} // namespace test_seam

// Register authentication routes (bootstrap, registration policy/invites,
// login, recovery, logout, sessions). Bootstrap and ordinary registration are
// separate transactions; only the secret-authorized bootstrap may grant the
// first user administrator membership.
// Call from main() after Drogon is configured but before app().run().
auto register_auth_routes(bool dev_mode,
                          const Config::Registration& registration,
                          const std::string& bootstrap_token) -> void;

// Register PAT routes (create, list, revoke).
// Call from main() after Drogon is configured but before app().run().
auto register_pat_routes() -> void;

} // namespace plinth::auth
