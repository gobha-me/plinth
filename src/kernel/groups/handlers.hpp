#pragma once

#include "kernel/config.hpp"

#include <string>

namespace plinth::groups {

// Seed built-in groups (admin, everyone), the kernel.admin rule,
// and the admin→kernel.admin grant.  Called once from main() after
// bootstrap_schema() but before Drogon starts.  Uses direct libpq.
// Idempotent — safe to call on every startup.
auto bootstrap_groups(const Config::Database& db_cfg) -> void;

// Register all group and RBAC HTTP routes with Drogon.
// Idempotent per process: the first call registers the routes and their
// rule requirements; subsequent calls are no-ops. Production calls this
// once from main() before `app().run()`; the grouped Catch2 subprocess
// model (0.4.5.1) calls it from multiple TEST_CASEs, where the idempotency
// guard prevents drogon's `!routersInit_` assertion from firing after the
// kernel has begun listening.
auto register_group_routes() -> void;

} // namespace plinth::groups
