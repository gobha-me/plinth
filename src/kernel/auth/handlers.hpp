#pragma once

namespace plinth::auth {

// Register authentication routes (register, login, logout, sessions).
// First-user selection, creation, and admin membership commit atomically.
// Concurrent disabled-registration requests have one bootstrap winner; with
// registration enabled, later users register without bootstrap privileges.
// Call from main() after Drogon is configured but before app().run().
auto register_auth_routes(bool dev_mode, bool registration_enabled) -> void;

// Register PAT routes (create, list, revoke).
// Call from main() after Drogon is configured but before app().run().
auto register_pat_routes() -> void;

} // namespace plinth::auth
