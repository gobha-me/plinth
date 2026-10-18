#pragma once

#include "kernel/auth/middleware.hpp"
#include "kernel/frontend/typed_requests.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace plinth::frontend::admin_defaults {

using Error = TypedRequestError;
inline constexpr std::size_t MAX_MUTATION_BYTES = 2048;
inline constexpr std::size_t MAX_QUERY_BYTES = 16384;
inline constexpr std::size_t MAX_CURSOR_BYTES = 4096;
inline constexpr std::size_t MAX_RESPONSE_BYTES = 131072;
inline constexpr std::size_t MAX_PAGE_ENTRIES = 64;

struct Handler {
  std::string application_id;
  std::string panel_id;
  auto operator==(const Handler&) const -> bool = default;
};
struct ReadSingle {
  std::string content_type;
  auto operator==(const ReadSingle&) const -> bool = default;
};
struct ReadPage {
  std::size_t limit = MAX_PAGE_ENTRIES;
  std::optional<std::string> cursor;
  auto operator==(const ReadPage&) const -> bool = default;
};
using ReadRequest = std::variant<ReadSingle, ReadPage>;
struct PutRequest {
  std::string content_type;
  Handler handler;
  auto operator==(const PutRequest&) const -> bool = default;
};
struct DeleteRequest {
  std::string content_type;
  auto operator==(const DeleteRequest&) const -> bool = default;
};

// Syntax only, never fresh management authority or storage admission. The HTTP
// adapter must additionally refuse GET bodies and mutation queries. The empty
// query is page(limit64); percent decoding happens exactly once here, including
// duplicate detection on decoded names. Raw byte excess is TOO_LARGE (413),
// malformed/structural input INVALID (400), allocation/length pressure
// UNAVAILABLE (503). The mutation SAX preflight precedes DOM construction.
[[nodiscard]] auto parse_read_query(std::string_view raw)
    -> std::expected<ReadRequest, Error>;
[[nodiscard]] auto parse_put(std::string_view raw)
    -> std::expected<PutRequest, Error>;
[[nodiscard]] auto parse_delete(std::string_view raw)
    -> std::expected<DeleteRequest, Error>;
// Apply these same syntax bounds to native callers before admission/dispatch.
[[nodiscard]] auto validate_read(const ReadRequest& request)
    -> std::expected<void, Error>;
[[nodiscard]] auto validate_put(const PutRequest& request)
    -> std::expected<void, Error>;
[[nodiscard]] auto validate_delete(const DeleteRequest& request)
    -> std::expected<void, Error>;

struct Entry {
  std::string content_type;
  Handler handler;
  auto operator==(const Entry&) const -> bool = default;
};
struct SingleResult {
  std::optional<Handler> handler;
};
struct PageResult {
  std::vector<Entry> entries;
  // Internal fixed-module position, never a browser cursor or authority.
  std::optional<std::string> next_after;
};
struct PutResult {
  bool saved = false;
};
struct DeleteResult {
  bool deleted = false;
};

// Closed fixed-module v1 shapes; malformed/oversized results are UNAVAILABLE,
// never absence, partial success or a caller input refusal. Preserve logical
// choices even if their application is currently uninstalled/ineligible.
[[nodiscard]] auto parse_single_result(std::string_view raw)
    -> std::expected<SingleResult, Error>;
[[nodiscard]] auto parse_page_result(
    std::string_view raw, std::size_t limit,
    std::optional<std::string_view> after = std::nullopt)
    -> std::expected<PageResult, Error>;
[[nodiscard]] auto parse_put_result(std::string_view raw)
    -> std::expected<PutResult, Error>;
[[nodiscard]] auto parse_delete_result(std::string_view raw)
    -> std::expected<DeleteResult, Error>;

// Bounded external schema1 envelopes with nested handlers. No permission,
// runtime lease, DB acknowledgement or profile fence is created by encoding.
// Service must pass only actual successful module results, pin the admitted
// profile through validation/signing, and freshly fence that same identity
// before response handoff. A write refusal after settlement can mean unknown
// commit outcome; these helpers provide neither rollback nor retry authority.
[[nodiscard]] auto encode_single_result(std::string_view content_type,
                                        const SingleResult& result)
    -> std::expected<nlohmann::json, Error>;
[[nodiscard]] auto encode_page_result(
    const PageResult& result,
    std::optional<std::string_view> next_cursor = std::nullopt)
    -> std::expected<nlohmann::json, Error>;
[[nodiscard]] auto encode_put_result(const PutResult& result)
    -> std::expected<nlohmann::json, Error>;
[[nodiscard]] auto encode_delete_result(const DeleteResult& result)
    -> std::expected<nlohmann::json, Error>;

struct CursorBinding {
  std::size_t limit = MAX_PAGE_ENTRIES;
  // Exact admitted profile, not a later global-current profile. This value
  // does not prove current identity; the service owns its final ACK fence.
  std::string platform_profile;
};

class CursorCodec;
namespace test_seam {
// Deterministic crypto vectors only; confers no management permission.
[[nodiscard]] auto cursor_codec(std::array<unsigned char, 32> secret)
    -> CursorCodec;
} // namespace test_seam

// Separate administrative domain/key/position, not a fabricated SurfaceKind
// or float rank. Each service/process instance owns one CSPRNG key. The MAC
// binds original user/auth kind/credential source fingerprint, admitted
// profile, page limit, MIME after and exact sixty-second lifetime. Pages are
// independently fresh; the cursor promises no transactional snapshot.
class CursorCodec {
 public:
  CursorCodec(); // CSPRNG failure throws; no fallback key.
  ~CursorCodec();
  CursorCodec(const CursorCodec&) = delete;
  auto operator=(const CursorCodec&) -> CursorCodec& = delete;
  CursorCodec(CursorCodec&&) = delete;
  auto operator=(CursorCodec&&) -> CursorCodec& = delete;

  [[nodiscard]] auto encode(const auth::AuthContext& auth,
                            const CursorBinding& binding,
                            std::string_view after,
                            std::int64_t unix_seconds) const
      -> std::expected<std::string, Error>;
  [[nodiscard]] auto decode(std::string_view raw, const auth::AuthContext& auth,
                            const CursorBinding& binding,
                            std::int64_t unix_seconds) const
      -> std::expected<std::string, Error>;

 private:
  friend auto test_seam::cursor_codec(std::array<unsigned char, 32> secret)
      -> CursorCodec;
  explicit CursorCodec(std::array<unsigned char, 32> secret);
  std::array<unsigned char, 32> secret;
};

} // namespace plinth::frontend::admin_defaults
