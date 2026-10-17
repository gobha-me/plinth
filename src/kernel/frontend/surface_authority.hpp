#pragma once

#include "kernel/auth/middleware.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/frontend/typed_requests.hpp"
#include "kernel/packages/panels_manifest.hpp"

#include <cstddef>
#include <expected>
#include <libpq-fe.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::frontend::authority {

enum class Error { INVALID, UNAUTHENTICATED, DENIED, UNAVAILABLE };

struct SurfaceTarget {
  std::string generation;
  std::string application_id;
  std::string version;
  SurfaceKind kind;
  packages::PanelEntry declaration;
  std::optional<std::string> logical_install_order;
  std::string capability_rule;
  std::string provider_type;
  std::string provider_name;
  std::string capability_scope;
};

struct Position {
  std::string logical_install_order;
  std::string application_id;
  std::string panel_id;
  auto operator==(const Position&) const -> bool = default;
};

struct LookupResult {
  capabilities::UserContext caller;
  std::optional<SurfaceTarget> target;
};
struct SurfacePage {
  capabilities::UserContext caller;
  std::vector<SurfaceTarget> targets;
  std::optional<Position> next_position;
};

// Caller owns PGconn, transaction/deadline/cancellation and lifecycle locks.
// No IO owner or connection is retained. Every query checks the exact current
// credential and effective rules in the SAME PostgreSQL statement snapshot.
// Returned data is not captured-asset/readiness/persistence admission.
[[nodiscard]] auto authenticate(PGconn& connection,
                                const auth::AuthContext& auth)
    -> std::expected<capabilities::UserContext, Error>;
[[nodiscard]] auto require_rule(PGconn& connection,
                                const auth::AuthContext& auth,
                                std::string_view rule)
    -> std::expected<capabilities::UserContext, Error>;
// Exact current shell:1:preferences.get/set provider/rule binding plus the
// existing shell.preferences.read/write rule. Admin remains universal, but
// cannot synthesize a missing/disabled/foreign registered storage profile.
[[nodiscard]] auto require_shell_preference_rule(PGconn& connection,
                                                 const auth::AuthContext& auth,
                                                 bool write)
    -> std::expected<capabilities::UserContext, Error>;
[[nodiscard]] auto reviewer_is_admin(PGconn& connection,
                                     std::string_view user_id)
    -> std::expected<bool, Error>;

// Missing/denied targets carry no metadata. Expected generation/capability and
// requested MIME are deliberately compared by the owning admission service,
// after this fresh logical lookup; no invented public lookup variant is added.
[[nodiscard]] auto lookup_surface(PGconn& connection,
                                  const auth::AuthContext& auth,
                                  std::string_view application_id,
                                  std::string_view panel_id, SurfaceKind kind)
    -> std::expected<LookupResult, Error>;
// Exact global numeric ordinal/app-byte/panel-byte prefix, auth filtered BEFORE
// LIMIT, with one lookahead only. Cursor MAC and subsequent physical readiness
// belong to the service; a cold/busy earlier candidate must not be skipped.
[[nodiscard]] auto catalog_float_prefix(
    PGconn& connection, const auth::AuthContext& auth,
    std::string_view content_type, std::size_t limit,
    const std::optional<Position>& after = std::nullopt)
    -> std::expected<SurfacePage, Error>;
[[nodiscard]] auto catalog_float_prefix(
    PGconn& connection, const auth::AuthContext& auth,
    std::nullopt_t content_type, std::size_t limit,
    const std::optional<Position>& after = std::nullopt)
    -> std::expected<SurfacePage, Error>;

// Current shipping schema cannot attach user ownership to scope=user rows.
// Such a stored row denies that signature, never falls back to instance.
// No per-user semantics are fabricated from test-only cache seed identities.

} // namespace plinth::frontend::authority
