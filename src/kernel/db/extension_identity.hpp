// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/auth/crypto.hpp"

#include <string>
#include <string_view>

namespace plinth::db {

// PostgreSQL roles are cluster-wide and identifiers are limited to 63 bytes.
// Keep the full extension identity in kernel metadata and use a deterministic
// database-scoped 240-bit digest for the login identifier.
inline auto extension_role_name(std::string_view database,
                                std::string_view extension) -> std::string {
  return "px_" +
         auth::sha256_hex(std::string{database} + ":" + std::string{extension})
             .substr(0, 60);
}

} // namespace plinth::db
