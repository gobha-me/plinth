#pragma once

#include "kernel/config.hpp"

#include <string>

namespace plinth::db {

// Read migrations/schema.sql from the given directory.
// Throws std::runtime_error if the file is missing or empty.
auto load_schema_sql(const std::string& migrations_dir) -> std::string;

// Connect to PG, bootstrap the plinth schema, disconnect.
//   dev_mode=true: drop the plinth schema plus Plinth-owned `ext_*` schemas and
//                  roles, then run schema.sql
//   dev_mode=false: if plinth schema doesn't exist, run schema.sql (fresh
//   install)
// Throws std::runtime_error on connection or DDL failure.
auto bootstrap_schema(const Config::Database& db_cfg,
                      const std::string& migrations_dir, bool dev_mode) -> void;

} // namespace plinth::db
