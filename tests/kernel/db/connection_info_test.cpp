// SPDX-License-Identifier: MIT

#include "kernel/db/connection_info.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <string_view>

namespace {

auto require_round_trip(const plinth::Config::Database& db) -> void {
  char* error = nullptr;
  using Options = std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)>;
  Options options{
      PQconninfoParse(plinth::db::connection_info(db).c_str(), &error),
      PQconninfoFree};
  std::unique_ptr<char, decltype(&PQfreemem)> error_guard{error, PQfreemem};
  REQUIRE(error == nullptr);
  REQUIRE(options != nullptr);
  auto find = [&options](std::string_view keyword) -> std::string {
    for (auto* entry = options.get(); entry->keyword != nullptr; ++entry) {
      if (keyword == entry->keyword) {
        REQUIRE(entry->val != nullptr);
        return entry->val;
      }
    }
    FAIL("missing PostgreSQL connection option");
    return {};
  };
  REQUIRE(find("host") == db.host);
  REQUIRE(find("port") == std::to_string(db.port));
  REQUIRE(find("dbname") == db.database);
  REQUIRE(find("user") == db.user);
  REQUIRE(find("password") == db.password);
}

} // namespace

TEST_CASE("PostgreSQL connection values round trip through libpq",
          "[db][conninfo][unit]") {
  plinth::Config::Database db;
  require_round_trip(db);
  constexpr std::array<std::string_view, 9> VALUES{
      "",
      "two words",
      "back\\slash",
      "single'quote",
      "double\"quote",
      "\t\n\r\f\v",
      " leading and trailing ",
      R"(x' host=other password=other \)",
      "unicode-\xc3\xa9"};
  for (auto value : VALUES) {
    db.host = value;
    db.database = value;
    db.user = value;
    db.password = value;
    require_round_trip(db);
  }
}

TEST_CASE("PostgreSQL connection values reject embedded NUL without secrets",
          "[db][conninfo][unit]") {
  using Field = std::string plinth::Config::Database::*;
  constexpr std::array<Field, 4> FIELDS{
      &plinth::Config::Database::host, &plinth::Config::Database::database,
      &plinth::Config::Database::user, &plinth::Config::Database::password};
  for (auto field : FIELDS) {
    plinth::Config::Database db;
    db.*field = std::string{"fake-secret\0suffix", 18};
    REQUIRE_THROWS_WITH(plinth::db::connection_info(db),
                        "PostgreSQL connection parameter contains a NUL byte");
  }
}
