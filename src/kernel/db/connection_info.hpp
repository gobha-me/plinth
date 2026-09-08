// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/config.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace plinth::db {

// Every libpq keyword value is quoted, including empty strings. libpq
// requires a backslash before literal quotes and backslashes inside quotes.
// Do not include this string in diagnostics: it contains the password.
[[nodiscard]] inline auto connection_info(const Config::Database& config)
    -> std::string {
  auto quote = [](std::string_view value) {
    if (value.contains('\0')) {
      throw std::invalid_argument(
          "PostgreSQL connection parameter contains a NUL byte");
    }
    std::string result{"'"};
    for (char ch : value) {
      if (ch == '\'' || ch == '\\') {
        result += '\\';
      }
      result += ch;
    }
    result += '\'';
    return result;
  };
  return "host=" + quote(config.host) + " port=" + std::to_string(config.port) +
         " dbname=" + quote(config.database) + " user=" + quote(config.user) +
         " password=" + quote(config.password);
}

} // namespace plinth::db
