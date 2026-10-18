#include "kernel/frontend/admin_defaults_contract.hpp"

#include "kernel/packages/detail/launcher_validation.hpp"
#include "kernel/packages/detail/restore_json.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace plinth::frontend::admin_defaults {
namespace {
using Json = nlohmann::json;
constexpr std::string_view ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view CURSOR_DOMAIN = "plinth.admin-defaults.cursor.v1";
constexpr std::string_view CREDENTIAL_DOMAIN =
    "plinth.admin-defaults.credential.v1";
constexpr std::string_view CURSOR_KIND = "admin_defaults";

// Same full-string native ID grammar as typed_requests/surface_protocol. Those
// translation-unit-private helpers cannot be called without changing their
// contracts. In particular, no regex '$' trailing-newline acceptance.
auto identifier(std::string_view text, bool application) -> bool {
  return text.size() >= (application ? 2U : 1U) && text.size() <= 64 &&
         text.front() >= 'a' && text.front() <= 'z' &&
         std::ranges::all_of(text, [application](char byte) {
           return (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') || byte == '-' ||
                  (!application && byte == '_');
         });
}
auto valid_handler(const Handler& handler) -> bool {
  return identifier(handler.application_id, true) &&
         identifier(handler.panel_id, false);
}
auto exact(const Json& value, std::initializer_list<std::string_view> keys)
    -> bool {
  return value.is_object() && value.size() == keys.size() &&
         std::ranges::all_of(keys,
                             [&](auto key) { return value.contains(key); });
}
auto plain_text(std::string_view value) -> bool {
  return packages::detail::unicode_scalar_count(value).has_value() &&
         std::ranges::none_of(value, [](char byte) {
           return static_cast<unsigned char>(byte) < 0x20U || byte == '\x7f';
         });
}
auto cursor_text(std::string_view value) -> bool {
  return !value.empty() && value.size() <= MAX_CURSOR_BYTES &&
         std::ranges::all_of(value, [](char byte) {
           return byte == '.' || ALPHABET.contains(byte);
         });
}
auto page_limit(std::size_t limit) -> bool {
  return limit >= 1 && limit <= MAX_PAGE_ENTRIES;
}
auto handler_of(const Json& value) -> std::optional<Handler> {
  if (!exact(value, {"application_id", "panel_id"}) ||
      !value["application_id"].is_string() || !value["panel_id"].is_string())
    return std::nullopt;
  const auto& app = value["application_id"].get_ref<const std::string&>();
  const auto& panel = value["panel_id"].get_ref<const std::string&>();
  if (!identifier(app, true) || !identifier(panel, false)) return std::nullopt;
  return Handler{app, panel};
}
auto handler_json(const Handler& handler) -> Json {
  return {{"application_id", handler.application_id},
          {"panel_id", handler.panel_id}};
}

// Raw bytes are bounded BEFORE this SAX pass. It retains decoded object keys
// and caps depth and value nodes BEFORE constructing a DOM, including unknown
// fields. Scalar strings cannot smuggle NUL/control aliases through native
// JSON construction. Mutation/cursor profiles do not accept arrays.
class BoundedSax final : public nlohmann::json_sax<Json> {
 public:
  BoundedSax(std::size_t depth, std::size_t nodes, bool arrays)
      : max_depth(depth), max_nodes(nodes), allow_arrays(arrays) {}
  auto null() -> bool override { return value(); }
  auto boolean(bool) -> bool override { return value(); }
  auto number_integer(number_integer_t) -> bool override { return value(); }
  auto number_unsigned(number_unsigned_t) -> bool override { return value(); }
  auto number_float(number_float_t, const string_t&) -> bool override {
    return false;
  }
  auto string(string_t& text) -> bool override {
    return value() && plain_text(text);
  }
  auto binary(binary_t&) -> bool override { return false; }
  auto start_object(std::size_t) -> bool override { return begin(true); }
  auto start_array(std::size_t) -> bool override {
    return allow_arrays && begin(false);
  }
  auto key(string_t& text) -> bool override {
    return !frames.empty() && frames.back().object && plain_text(text) &&
           frames.back().keys.insert(text).second;
  }
  auto end_object() -> bool override { return end(true); }
  auto end_array() -> bool override { return end(false); }
  auto parse_error(std::size_t, const std::string&, const Json::exception&)
      -> bool override {
    return false;
  }

 private:
  struct Frame {
    bool object;
    std::set<std::string> keys;
  };
  const std::size_t max_depth;
  const std::size_t max_nodes;
  const bool allow_arrays;
  std::size_t nodes = 0;
  std::vector<Frame> frames;
  auto value() -> bool {
    return frames.size() < max_depth && ++nodes <= max_nodes;
  }
  auto begin(bool object) -> bool {
    if (!value()) return false;
    frames.push_back({object, {}});
    return true;
  }
  auto end(bool object) -> bool {
    if (frames.empty() || frames.back().object != object) return false;
    frames.pop_back();
    return true;
  }
};

auto parse_json(std::string_view raw, std::size_t cap, Error malformed,
                bool arrays, std::size_t depth, std::size_t nodes)
    -> std::expected<Json, Error> {
  if (raw.size() > cap)
    return std::unexpected(malformed == Error::INVALID ? Error::TOO_LARGE
                                                       : Error::UNAVAILABLE);
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<Json, Error> {
        try {
          if (raw.find('\0') != std::string_view::npos)
            return std::unexpected(malformed);
          BoundedSax preflight{depth, nodes, arrays};
          if (!Json::sax_parse(raw.begin(), raw.end(), &preflight))
            return std::unexpected(malformed);
          return Json::parse(raw.begin(), raw.end());
        } catch (const Json::exception&) {
          return std::unexpected(malformed);
        }
      });
}
auto mutation_json(std::string_view raw) -> std::expected<Json, Error> {
  return parse_json(raw, MAX_MUTATION_BYTES, Error::INVALID, false, 3, 32);
}
auto module_json(std::string_view raw) -> std::expected<Json, Error> {
  return parse_json(raw, MAX_RESPONSE_BYTES, Error::UNAVAILABLE, true, 4, 512);
}
auto schema_one(const Json& value) -> bool {
  return value.is_number_integer() && value == 1;
}
auto mime_field(const Json& root) -> bool {
  return root["content_type"].is_string() &&
         is_canonical_content_type(
             root["content_type"].get_ref<const std::string&>());
}
auto hex(char byte) -> int {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}
auto decode_query_part(std::string_view raw) -> std::optional<std::string> {
  std::string decoded;
  decoded.reserve(raw.size());
  for (std::size_t index = 0; index < raw.size(); ++index) {
    if (raw[index] == '%') {
      if (index + 2 >= raw.size()) return std::nullopt;
      const int high = hex(raw[index + 1]), low = hex(raw[index + 2]);
      if (high < 0 || low < 0) return std::nullopt;
      decoded.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else {
      decoded.push_back(raw[index] == '+' ? ' ' : raw[index]);
    }
  }
  return plain_text(decoded) ? std::optional{std::move(decoded)} : std::nullopt;
}
auto byte_less(std::string_view left, std::string_view right) -> bool {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
        return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
      });
}
auto valid_page(const PageResult& page, std::size_t limit,
                std::optional<std::string_view> after) -> bool {
  if (!page_limit(limit) || page.entries.size() > limit ||
      (after && !is_canonical_content_type(*after)))
    return false;
  std::string_view previous = after.value_or("");
  for (const auto& entry : page.entries) {
    if (!is_canonical_content_type(entry.content_type) ||
        !valid_handler(entry.handler) ||
        !byte_less(previous, entry.content_type))
      return false;
    previous = entry.content_type;
  }
  // A lookahead exists only for a full page, and its continuation is exactly
  // the final returned row, not the unseen row or a raw caller-supplied key.
  return !page.next_after ||
         (page.entries.size() == limit &&
          *page.next_after == page.entries.back().content_type);
}
auto envelope(Json value) -> std::expected<Json, Error> {
  if (!packages::restore::detail::canonical_json(value, MAX_RESPONSE_BYTES))
    return std::unexpected(Error::UNAVAILABLE);
  return value;
}
auto digest(std::string_view text) -> bool {
  return text.size() == 64 && std::ranges::all_of(text, [](char byte) {
           return (byte >= 'a' && byte <= 'f') || (byte >= '0' && byte <= '9');
         });
}
auto uuid(std::string_view text) -> bool {
  if (text.size() != 36) return false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (text[index] != '-') return false;
    } else if (!((text[index] >= '0' && text[index] <= '9') ||
                 (text[index] >= 'a' && text[index] <= 'f')))
      return false;
  }
  return true;
}
auto valid_binding(const CursorBinding& binding) -> bool {
  const std::string_view profile = binding.platform_profile;
  return page_limit(binding.limit) && profile.size() == 104 &&
         profile.starts_with("v1:") && uuid(profile.substr(3, 36)) &&
         profile[39] == ':' && digest(profile.substr(40));
}
auto valid_auth(const auth::AuthContext& auth) -> bool {
  return uuid(auth.user_id) && digest(auth.token_hash) &&
         ((auth.auth_type == "session" && uuid(auth.session_id) &&
           auth.pat_id.empty() &&
           (auth.credential_source == auth::CredentialSource::COOKIE ||
            auth.credential_source == auth::CredentialSource::BEARER)) ||
          (auth.auth_type == "pat" && uuid(auth.pat_id) &&
           auth.session_id.empty() &&
           auth.credential_source == auth::CredentialSource::BEARER));
}
// Same reviewed OpenSSL primitives/framing and canonical base64url as the
// surface cursor, with independent administrative domains and owning key.
auto base64url(std::string_view raw) -> std::string {
  std::string result;
  result.reserve((raw.size() * 4 + 2) / 3);
  for (std::size_t index = 0; index < raw.size(); index += 3) {
    const auto first = static_cast<unsigned char>(raw[index]);
    const auto second =
        index + 1 < raw.size() ? static_cast<unsigned char>(raw[index + 1]) : 0;
    const auto third =
        index + 2 < raw.size() ? static_cast<unsigned char>(raw[index + 2]) : 0;
    result.push_back(ALPHABET[first >> 2U]);
    result.push_back(ALPHABET[((first & 3U) << 4U) | (second >> 4U)]);
    if (index + 1 < raw.size())
      result.push_back(ALPHABET[((second & 15U) << 2U) | (third >> 6U)]);
    if (index + 2 < raw.size()) result.push_back(ALPHABET[third & 63U]);
  }
  return result;
}
auto decode_base64url(std::string_view raw) -> std::optional<std::string> {
  if (raw.empty() || raw.size() > MAX_CURSOR_BYTES || raw.size() % 4 == 1)
    return std::nullopt;
  std::string result;
  result.reserve(raw.size() * 3 / 4);
  unsigned int bits = 0, count = 0;
  for (char byte : raw) {
    const auto value = ALPHABET.find(byte);
    if (value == std::string_view::npos) return std::nullopt;
    bits = (bits << 6U) | static_cast<unsigned int>(value);
    count += 6;
    if (count >= 8) {
      count -= 8;
      result.push_back(static_cast<char>((bits >> count) & 255U));
    }
    bits &= (1U << count) - 1U;
  }
  if (bits != 0 || base64url(result) != raw) return std::nullopt;
  return result;
}
auto hmac(const std::array<unsigned char, 32>& secret, std::string_view domain,
          std::string_view raw) -> std::expected<std::string, Error> {
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
      EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, nullptr, secret.data(),
                                   secret.size()),
      EVP_PKEY_free};
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
      EVP_MD_CTX_new(), EVP_MD_CTX_free};
  std::array<unsigned char, 32> result{};
  std::size_t size = result.size();
  constexpr char NUL = '\0';
  if (!key || !context ||
      EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr,
                         key.get()) != 1 ||
      EVP_DigestSignUpdate(context.get(), domain.data(), domain.size()) != 1 ||
      EVP_DigestSignUpdate(context.get(), &NUL, 1) != 1 ||
      EVP_DigestSignUpdate(context.get(), raw.data(), raw.size()) != 1 ||
      EVP_DigestSignFinal(context.get(), result.data(), &size) != 1 ||
      size != result.size())
    return std::unexpected(Error::UNAVAILABLE);
  return std::string{reinterpret_cast<const char*>(result.data()),
                     result.size()};
}
auto fingerprint(const std::array<unsigned char, 32>& secret,
                 const auth::AuthContext& auth)
    -> std::expected<std::string, Error> {
  const Json source{
      {"auth_type", auth.auth_type},
      {"source", auth.credential_source == auth::CredentialSource::COOKIE
                     ? "cookie"
                     : "bearer"},
      {"token_hash", auth.token_hash}};
  const auto bytes = packages::restore::detail::canonical_json(source, 256);
  if (!bytes) return std::unexpected(Error::UNAVAILABLE);
  const auto mac = hmac(secret, CREDENTIAL_DOMAIN, *bytes);
  if (!mac) return std::unexpected(mac.error());
  return base64url(*mac);
}
auto seconds(const Json& value) -> std::optional<std::int64_t> {
  if (!value.is_number_integer()) return std::nullopt;
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return std::nullopt;
    return static_cast<std::int64_t>(number);
  }
  const auto number = value.get<std::int64_t>();
  return number >= 0 ? std::optional{number} : std::nullopt;
}
} // namespace

auto parse_read_query(std::string_view raw)
    -> std::expected<ReadRequest, Error> {
  if (raw.size() > MAX_QUERY_BYTES) return std::unexpected(Error::TOO_LARGE);
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<ReadRequest, Error> {
        if (raw.empty()) return ReadPage{};
        std::map<std::string, std::string> fields;
        for (std::size_t begin = 0; begin <= raw.size();) {
          const auto amp = raw.find('&', begin);
          const auto item = raw.substr(
              begin, amp == std::string_view::npos ? amp : amp - begin);
          const auto equal = item.find('=');
          if (equal == std::string_view::npos || equal == 0 ||
              item.find('=', equal + 1) != std::string_view::npos)
            return std::unexpected(Error::INVALID);
          auto key = decode_query_part(item.substr(0, equal));
          auto value = decode_query_part(item.substr(equal + 1));
          if (!key || !value ||
              (*key != "content_type" && *key != "limit" && *key != "cursor") ||
              !fields.emplace(std::move(*key), std::move(*value)).second)
            return std::unexpected(Error::INVALID);
          if (amp == std::string_view::npos) break;
          begin = amp + 1;
        }
        if (const auto type = fields.find("content_type");
            type != fields.end()) {
          if (fields.size() != 1 || !is_canonical_content_type(type->second))
            return std::unexpected(Error::INVALID);
          return ReadSingle{type->second};
        }
        ReadPage page;
        if (const auto found = fields.find("limit"); found != fields.end()) {
          const auto& text = found->second;
          const auto converted = std::from_chars(
              text.data(), text.data() + text.size(), page.limit);
          if (text.empty() || text.front() == '0' ||
              converted.ec != std::errc{} ||
              converted.ptr != text.data() + text.size() ||
              !page_limit(page.limit))
            return std::unexpected(Error::INVALID);
        }
        if (const auto found = fields.find("cursor"); found != fields.end()) {
          if (!cursor_text(found->second))
            return std::unexpected(Error::INVALID);
          page.cursor = found->second;
        }
        return page;
      });
}
auto parse_put(std::string_view raw) -> std::expected<PutRequest, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<PutRequest, Error> {
        auto root = mutation_json(raw);
        if (!root) return std::unexpected(root.error());
        if (!exact(*root, {"schema_version", "content_type", "handler"}) ||
            !schema_one((*root)["schema_version"]) || !mime_field(*root))
          return std::unexpected(Error::INVALID);
        auto handler = handler_of((*root)["handler"]);
        if (!handler) return std::unexpected(Error::INVALID);
        return PutRequest{(*root)["content_type"].get<std::string>(),
                          std::move(*handler)};
      });
}
auto parse_delete(std::string_view raw) -> std::expected<DeleteRequest, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<DeleteRequest, Error> {
        auto root = mutation_json(raw);
        if (!root) return std::unexpected(root.error());
        if (!exact(*root, {"schema_version", "content_type"}) ||
            !schema_one((*root)["schema_version"]) || !mime_field(*root))
          return std::unexpected(Error::INVALID);
        return DeleteRequest{(*root)["content_type"].get<std::string>()};
      });
}
auto validate_read(const ReadRequest& request) -> std::expected<void, Error> {
  if (const auto* single = std::get_if<ReadSingle>(&request)) {
    if (is_canonical_content_type(single->content_type)) return {};
  } else if (const auto* page = std::get_if<ReadPage>(&request)) {
    if (page_limit(page->limit) &&
        (!page->cursor || cursor_text(*page->cursor)))
      return {};
  }
  return std::unexpected(Error::INVALID);
}
auto validate_put(const PutRequest& request) -> std::expected<void, Error> {
  if (is_canonical_content_type(request.content_type) &&
      valid_handler(request.handler))
    return {};
  return std::unexpected(Error::INVALID);
}
auto validate_delete(const DeleteRequest& request)
    -> std::expected<void, Error> {
  if (is_canonical_content_type(request.content_type)) return {};
  return std::unexpected(Error::INVALID);
}

auto parse_single_result(std::string_view raw)
    -> std::expected<SingleResult, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<SingleResult, Error> {
        auto root = module_json(raw);
        if (!root) return std::unexpected(root.error());
        if (!exact(*root, {"candidate"}))
          return std::unexpected(Error::UNAVAILABLE);
        if ((*root)["candidate"].is_null()) return SingleResult{};
        auto handler = handler_of((*root)["candidate"]);
        if (!handler) return std::unexpected(Error::UNAVAILABLE);
        return SingleResult{std::move(handler)};
      });
}
auto parse_page_result(std::string_view raw, std::size_t limit,
                       std::optional<std::string_view> after)
    -> std::expected<PageResult, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<PageResult, Error> {
        if (!page_limit(limit) || (after && !is_canonical_content_type(*after)))
          return std::unexpected(Error::UNAVAILABLE);
        auto root = module_json(raw);
        if (!root) return std::unexpected(root.error());
        if (!exact(*root, {"entries", "next_after"}) ||
            !(*root)["entries"].is_array() || (*root)["entries"].size() > limit)
          return std::unexpected(Error::UNAVAILABLE);
        PageResult result;
        result.entries.reserve((*root)["entries"].size());
        for (const auto& row : (*root)["entries"]) {
          if (!exact(row, {"content_type", "application_id", "panel_id"}) ||
              !mime_field(row) || !row["application_id"].is_string() ||
              !row["panel_id"].is_string())
            return std::unexpected(Error::UNAVAILABLE);
          const auto& app = row["application_id"].get_ref<const std::string&>();
          const auto& panel = row["panel_id"].get_ref<const std::string&>();
          if (!identifier(app, true) || !identifier(panel, false))
            return std::unexpected(Error::UNAVAILABLE);
          result.entries.push_back(
              {row["content_type"].get<std::string>(), {app, panel}});
        }
        const auto& next = (*root)["next_after"];
        if (!next.is_null()) {
          if (!next.is_string()) return std::unexpected(Error::UNAVAILABLE);
          result.next_after = next.get<std::string>();
        }
        if (!valid_page(result, limit, after))
          return std::unexpected(Error::UNAVAILABLE);
        return result;
      });
}
auto parse_put_result(std::string_view raw) -> std::expected<PutResult, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<PutResult, Error> {
        auto root = module_json(raw);
        if (!root) return std::unexpected(root.error());
        if (!exact(*root, {"saved"}) || !(*root)["saved"].is_boolean() ||
            (*root)["saved"] != true)
          return std::unexpected(Error::UNAVAILABLE);
        return PutResult{true};
      });
}
auto parse_delete_result(std::string_view raw)
    -> std::expected<DeleteResult, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<DeleteResult, Error> {
        auto root = module_json(raw);
        if (!root) return std::unexpected(root.error());
        if (!exact(*root, {"deleted"}) || !(*root)["deleted"].is_boolean())
          return std::unexpected(Error::UNAVAILABLE);
        return DeleteResult{(*root)["deleted"].get<bool>()};
      });
}
auto encode_single_result(std::string_view type, const SingleResult& result)
    -> std::expected<Json, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<Json, Error> {
        if (!is_canonical_content_type(type) ||
            (result.handler && !valid_handler(*result.handler)))
          return std::unexpected(Error::UNAVAILABLE);
        return envelope(
            {{"schema_version", 1},
             {"content_type", type},
             {"handler",
              result.handler ? handler_json(*result.handler) : Json(nullptr)}});
      });
}
auto encode_page_result(const PageResult& result,
                        std::optional<std::string_view> cursor)
    -> std::expected<Json, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<Json, Error> {
        // A parsed page may have a caller limit below64. Validate continuation
        // against its actual count here; parse_page_result already checked
        // limit.
        const auto limit =
            result.next_after ? result.entries.size() : MAX_PAGE_ENTRIES;
        if (!valid_page(result, limit, std::nullopt) ||
            result.next_after.has_value() != cursor.has_value() ||
            (cursor && !cursor_text(*cursor)))
          return std::unexpected(Error::UNAVAILABLE);
        Json entries = Json::array();
        for (const auto& entry : result.entries)
          entries.push_back({{"content_type", entry.content_type},
                             {"handler", handler_json(entry.handler)}});
        return envelope(
            {{"schema_version", 1},
             {"entries", std::move(entries)},
             {"next_cursor", cursor ? Json(*cursor) : Json(nullptr)}});
      });
}
auto encode_put_result(const PutResult& result) -> std::expected<Json, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<Json, Error> {
        if (!result.saved) return std::unexpected(Error::UNAVAILABLE);
        return envelope({{"schema_version", 1}, {"saved", true}});
      });
}
auto encode_delete_result(const DeleteResult& result)
    -> std::expected<Json, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<Json, Error> {
        return envelope({{"schema_version", 1}, {"deleted", result.deleted}});
      });
}

CursorCodec::CursorCodec() {
  if (RAND_bytes(secret.data(), static_cast<int>(secret.size())) != 1)
    throw std::runtime_error("admin defaults cursor CSPRNG failed");
}
CursorCodec::CursorCodec(std::array<unsigned char, 32> key) : secret(key) {
}
CursorCodec::~CursorCodec() {
  OPENSSL_cleanse(secret.data(), secret.size());
}
auto test_seam::cursor_codec(std::array<unsigned char, 32> secret)
    -> CursorCodec {
  return CursorCodec{secret};
}
auto CursorCodec::encode(const auth::AuthContext& auth,
                         const CursorBinding& binding, std::string_view after,
                         std::int64_t now) const
    -> std::expected<std::string, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<std::string, Error> {
        if (!valid_auth(auth) || !valid_binding(binding) ||
            !is_canonical_content_type(after) || now < 0 ||
            now > std::numeric_limits<std::int64_t>::max() - 60)
          return std::unexpected(Error::INVALID);
        const auto credential = fingerprint(secret, auth);
        if (!credential) return std::unexpected(credential.error());
        const Json payload{{"v", 1},
                           {"user", auth.user_id},
                           {"credential", *credential},
                           {"kind", CURSOR_KIND},
                           {"limit", binding.limit},
                           {"platform_profile", binding.platform_profile},
                           {"after", after},
                           {"issued", now},
                           {"expires", now + 60}};
        const auto bytes = packages::restore::detail::canonical_json(
            payload, MAX_CURSOR_BYTES);
        if (!bytes) return std::unexpected(Error::UNAVAILABLE);
        const auto mac = hmac(secret, CURSOR_DOMAIN, *bytes);
        if (!mac) return std::unexpected(mac.error());
        auto result = base64url(*bytes) + "." + base64url(*mac);
        if (result.size() > MAX_CURSOR_BYTES)
          return std::unexpected(Error::UNAVAILABLE);
        return result;
      });
}
auto CursorCodec::decode(std::string_view raw, const auth::AuthContext& auth,
                         const CursorBinding& binding, std::int64_t now) const
    -> std::expected<std::string, Error> {
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<std::string, Error> {
        if (raw.size() > MAX_CURSOR_BYTES || !valid_auth(auth) ||
            !valid_binding(binding) || now < 0)
          return std::unexpected(Error::INVALID);
        const auto dot = raw.find('.');
        if (dot == std::string_view::npos ||
            raw.find('.', dot + 1) != std::string_view::npos)
          return std::unexpected(Error::INVALID);
        const auto bytes = decode_base64url(raw.substr(0, dot));
        const auto supplied = decode_base64url(raw.substr(dot + 1));
        if (!bytes || !supplied || supplied->size() != 32)
          return std::unexpected(Error::INVALID);
        const auto actual = hmac(secret, CURSOR_DOMAIN, *bytes);
        if (!actual) return std::unexpected(actual.error());
        if (CRYPTO_memcmp(actual->data(), supplied->data(), actual->size()) !=
            0)
          return std::unexpected(Error::INVALID);
        auto payload =
            parse_json(*bytes, MAX_CURSOR_BYTES, Error::INVALID, false, 2, 16);
        if (!payload) return std::unexpected(payload.error());
        if (!exact(*payload,
                   {"v", "user", "credential", "kind", "limit",
                    "platform_profile", "after", "issued", "expires"}))
          return std::unexpected(Error::INVALID);
        const auto canonical = packages::restore::detail::canonical_json(
            *payload, MAX_CURSOR_BYTES);
        if (!canonical) return std::unexpected(Error::UNAVAILABLE);
        if (*canonical != *bytes) return std::unexpected(Error::INVALID);
        const auto credential = fingerprint(secret, auth);
        if (!credential) return std::unexpected(credential.error());
        const auto& fingerprint_value = (*payload)["credential"];
        if (!schema_one((*payload)["v"]) ||
            (*payload)["user"] != auth.user_id ||
            !fingerprint_value.is_string() ||
            fingerprint_value.get_ref<const std::string&>().size() !=
                credential->size() ||
            CRYPTO_memcmp(
                fingerprint_value.get_ref<const std::string&>().data(),
                credential->data(), credential->size()) != 0 ||
            (*payload)["kind"] != CURSOR_KIND ||
            !(*payload)["limit"].is_number_integer() ||
            (*payload)["limit"] != binding.limit ||
            (*payload)["platform_profile"] != binding.platform_profile ||
            !(*payload)["after"].is_string() ||
            !is_canonical_content_type(
                (*payload)["after"].get_ref<const std::string&>()))
          return std::unexpected(Error::INVALID);
        const auto issued = seconds((*payload)["issued"]);
        const auto expires = seconds((*payload)["expires"]);
        if (!issued || !expires ||
            *issued > std::numeric_limits<std::int64_t>::max() - 60 ||
            *expires != *issued + 60 || now < *issued || now >= *expires)
          return std::unexpected(Error::INVALID);
        return (*payload)["after"].get<std::string>();
      });
}
} // namespace plinth::frontend::admin_defaults
