#pragma once

#include "kernel/auth/middleware.hpp"
#include "kernel/frontend/persistence_projection.hpp"
#include "kernel/frontend/surface_authority.hpp"
#include "kernel/packages/generation_capture.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace plinth::frontend::protocol {

enum class Error { INVALID, LIMIT_EXCEEDED, CRYPTO_FAILED };
enum class AssetKind { TYPED, PLATFORM };

// Decoded logical namespace only. Never a path/permission/admission token.
struct AssetSelector {
  AssetKind kind;
  std::string generation;
  std::string artifact_digest;
  std::optional<std::string> panel_id;
  std::string member_path; // Exact captured member, beginning client/.
  auto operator==(const AssetSelector&) const -> bool = default;
};

// Raw percent-encoded absolute path, not a framework-decoded/normalized path.
// Decode once and require RFC3986 canonical re-encoding. Literal decoded '%'
// remains literal; callers MUST NOT decode it again or resolve a filesystem.
[[nodiscard]] auto parse_asset_selector(std::string_view raw_path)
    -> std::expected<AssetSelector, Error>;

// Syntax/source identity serialization only: no eligibility, protected-policy
// approval, runtime readiness, lifecycle lease or authority is conferred.
// trusted_origin comes from configured/current server origin, NOT arbitrary
// request headers. Wire URL remains relative same-origin absolute path.
// Closed known fields are bounded before copies; final canonical JSON <=16KiB.
// This is a metadata profile, not a total-process allocator/capture budget.
[[nodiscard]] auto encode_float_target(
    const authority::SurfaceTarget& target,
    const packages::generation::CapturedArtifact& artifact,
    const std::optional<PersistenceProjection>& projection,
    std::string_view trusted_origin) -> std::expected<nlohmann::json, Error>;
[[nodiscard]] auto encode_primary_target(
    const authority::SurfaceTarget& target,
    const packages::generation::CapturedArtifact& artifact,
    std::string_view trusted_origin) -> std::expected<nlohmann::json, Error>;
// chrome_essential is supplied by the service AFTER fresh guarded active-shell
// identity proof. The declaration flag is never used by this serializer.
[[nodiscard]] auto encode_tray_target(
    const authority::SurfaceTarget& target,
    const packages::generation::CapturedArtifact& artifact,
    bool chrome_essential, std::string_view trusted_origin)
    -> std::expected<nlohmann::json, Error>;

struct CursorQuery {
  std::optional<std::string> content_type;
  std::size_t limit = 64;
  bool include_admin_default = false;
  std::string platform_profile;
  SurfaceKind kind = SurfaceKind::FLOAT;
  auto operator==(const CursorQuery&) const -> bool = default;
};

class CursorCodec;
namespace test_seam {
// Deterministic low-level crypto vectors only; no authorization/token factory.
[[nodiscard]] auto cursor_codec(std::array<unsigned char, 32> secret)
    -> CursorCodec;
} // namespace test_seam

// One secret per owning process/service instance. Restart invalidates cursors.
// Cursor validates only bounded pagination syntax, MAC and request binding;
// actual credential/RBAC/current generation must still be freshly checked.
class CursorCodec {
 public:
  CursorCodec(); // CSPRNG failure throws; never silently uses a fallback key.
  ~CursorCodec();
  CursorCodec(const CursorCodec&) = delete;
  auto operator=(const CursorCodec&) -> CursorCodec& = delete;
  CursorCodec(CursorCodec&&) = delete;
  auto operator=(CursorCodec&&) -> CursorCodec& = delete;
  [[nodiscard]] auto encode(const auth::AuthContext& auth,
                            const CursorQuery& query,
                            const authority::Position& position,
                            std::int64_t unix_seconds) const
      -> std::expected<std::string, Error>;
  [[nodiscard]] auto decode(std::string_view raw, const auth::AuthContext& auth,
                            const CursorQuery& query,
                            std::int64_t unix_seconds) const
      -> std::expected<authority::Position, Error>;

 private:
  friend auto test_seam::cursor_codec(std::array<unsigned char, 32> secret)
      -> CursorCodec;
  explicit CursorCodec(std::array<unsigned char, 32> secret);
  std::array<unsigned char, 32> secret;
};

} // namespace plinth::frontend::protocol
