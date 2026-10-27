#include "kernel/extension_http/codec.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <initializer_list>
#include <set>
#include <system_error>

namespace plinth::extension_http {
namespace {
using Json = nlohmann::ordered_json;
constexpr std::size_t MAX_BODY64 = ((MAX_BODY_BYTES + 2) / 3) * 4;
constexpr std::string_view BASE64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

auto invalid() -> std::unexpected<CodecError> {
  return std::unexpected(CodecError::INVALID);
}
auto too_large() -> std::unexpected<CodecError> {
  return std::unexpected(CodecError::TOO_LARGE);
}
auto lower_alpha(char c) -> bool {
  return c >= 'a' && c <= 'z';
}
auto digit(char c) -> bool {
  return c >= '0' && c <= '9';
}
auto alpha(char c) -> bool {
  return lower_alpha(c) || (c >= 'A' && c <= 'Z');
}
auto unreserved(char c) -> bool {
  return alpha(c) || digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
auto hex(char c) -> int {
  if (digit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
auto lower_hex(char c) -> bool {
  return digit(c) || (c >= 'a' && c <= 'f');
}
auto token(char c) -> bool {
  return alpha(c) || digit(c) ||
         std::string_view{"!#$%&'*+.^_`|~-"}.find(c) != std::string_view::npos;
}
auto hop_header(std::string_view name) -> bool {
  constexpr std::array names{"connection",
                             "keep-alive",
                             "proxy-connection",
                             "proxy-authenticate",
                             "proxy-authorization",
                             "te",
                             "trailer",
                             "transfer-encoding",
                             "upgrade"};
  return std::ranges::find(names, name) != names.end();
}
auto request_forbidden(std::string_view name) -> bool {
  return hop_header(name) || name == "cookie" || name == "authorization" ||
         name == "x-plinth-csrf" || name == "host" || name == "forwarded" ||
         name.starts_with("x-forwarded-");
}
auto response_forbidden(std::string_view name) -> bool {
  return hop_header(name) || name == "set-cookie" || name == "cookie" ||
         name == "authorization" || name == "www-authenticate" ||
         name == "authentication-info" || name == "proxy-authentication-info" ||
         name == "content-length" || name.starts_with("access-control-");
}
auto header_bytes(const Headers& headers) -> std::size_t {
  std::size_t size = 0;
  for (const auto& [name, value] : headers)
    size += name.size() + value.size();
  return size;
}
auto path_segments(std::string_view path) -> bool {
  if (path.empty() || path.front() != '/') return false;
  if (path == "/") return true;
  std::size_t start = 1;
  while (start < path.size()) {
    auto end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    const auto part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = end + 1;
  }
  return true; // One trailing slash is permitted for a request path.
}
auto matches(std::string_view claim, std::string_view path) -> bool {
  if (!claim.ends_with("/*")) return claim == path;
  claim.remove_suffix(2);
  return path == claim ||
         (path.starts_with(claim) && path.size() > claim.size() &&
          path[claim.size()] == '/');
}

// Schema-aware SAX prepass. No DOM exists while decoded keys, string totals,
// depth and node bounds are being checked. A key may be spelled with JSON
// escapes, but duplicates compare DECODED keys. Every stack frame is bounded
// by a closed record's key count (or the two header-array roles).
enum class Role {
  REQUEST,
  RESPONSE,
  CONTEXT,
  PRINCIPAL,
  EXTENSION,
  HEADERS,
  PAIR
};
auto allowed_key(Role role, std::string_view key) -> bool {
  switch (role) {
    case Role::REQUEST:
      return key == "schema" || key == "request_id" || key == "method" ||
             key == "path" || key == "original_path" || key == "query" ||
             key == "headers" || key == "body_b64";
    case Role::RESPONSE:
      return key == "schema" || key == "status" || key == "headers" ||
             key == "body_b64";
    case Role::CONTEXT:
      return key == "schema" || key == "dispatch_id" || key == "owner_epoch" ||
             key == "adapter_epoch" || key == "clock_domain_id" ||
             key == "admitted_at_ns" || key == "execution_cutoff_ns" ||
             key == "matched_prefix" || key == "principal" ||
             key == "extension";
    case Role::PRINCIPAL:
      return key == "kind" || key == "user_id" || key == "pat_id";
    case Role::EXTENSION:
      return key == "package_id" || key == "name" || key == "version" ||
             key == "artifact_sha256" || key == "generation_receipt_id";
    case Role::HEADERS:
    case Role::PAIR: return false;
  }
  return false;
}
class Preflight final : public nlohmann::json_sax<Json> {
 public:
  explicit Preflight(Role root) : root_role(root) {}
  auto null() -> bool override { return scalar(); }
  auto boolean(bool) -> bool override { return scalar(); }
  auto number_integer(number_integer_t) -> bool override { return scalar(); }
  auto number_unsigned(number_unsigned_t) -> bool override { return scalar(); }
  auto number_float(number_float_t, const string_t&) -> bool override {
    return scalar();
  }
  auto string(string_t& text) -> bool override {
    if (!scalar()) return false;
    if (stack.empty() || stack.back().key != "body_b64") {
      if (text.size() > MAX_METADATA_BYTES - metadata) {
        error = CodecError::TOO_LARGE;
        return false;
      }
      metadata += text.size();
    }
    return true;
  }
  auto binary(binary_t&) -> bool override { return false; }
  auto start_object(std::size_t) -> bool override {
    if (!value()) return false;
    Role next = root_role;
    if (!stack.empty()) {
      const auto& parent = stack.back();
      if (parent.role != Role::CONTEXT) return false;
      if (parent.key == "principal")
        next = Role::PRINCIPAL;
      else if (parent.key == "extension")
        next = Role::EXTENSION;
      else
        return false;
    }
    stack.push_back({next, {}, {}});
    return true;
  }
  auto key(string_t& key_text) -> bool override {
    if (stack.empty() || !allowed_key(stack.back().role, key_text) ||
        !stack.back().keys.insert(key_text).second)
      return false;
    stack.back().key = key_text;
    return true;
  }
  auto end_object() -> bool override { return pop(); }
  auto start_array(std::size_t) -> bool override {
    if (!value() || stack.empty()) return false;
    const auto& parent = stack.back();
    Role next = Role::PAIR;
    if (parent.role == Role::HEADERS)
      next = Role::PAIR;
    else if ((parent.role == Role::REQUEST || parent.role == Role::RESPONSE) &&
             parent.key == "headers")
      next = Role::HEADERS;
    else
      return false;
    stack.push_back({next, {}, {}});
    return true;
  }
  auto end_array() -> bool override { return pop(); }
  auto parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) -> bool override {
    return false;
  }
  CodecError error = CodecError::INVALID;

 private:
  struct Scope {
    Role role;
    std::set<std::string> keys;
    std::string key;
  };
  auto value() -> bool {
    if (stack.size() >= 8 || nodes == 1024) {
      error = CodecError::TOO_LARGE;
      return false;
    }
    ++nodes;
    return true;
  }
  auto scalar() -> bool { return value(); }
  auto pop() -> bool {
    if (stack.empty()) return false;
    stack.pop_back();
    return true;
  }
  Role root_role;
  std::vector<Scope> stack;
  std::size_t nodes = 0;
  std::size_t metadata = 0;
};

auto whitespace(char c) -> bool {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
// Inspect raw string tokens before SAX can allocate a large string. The only
// permitted bulk string is a literal, unescaped body_b64. Non-body encoded
// bytes (including whitespace, keys and escapes) have their separate ceiling.
auto encoded_bounds(std::string_view raw) -> CodecResult<void> {
  if (raw.empty()) return invalid();
  if (raw.size() > MAX_WIRE_BYTES) return too_large();
  if (!valid_utf8(raw)) return invalid();
  std::size_t body_bytes = 0;
  bool found_body = false;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '"') continue;
    const auto start = i;
    for (++i; i < raw.size() && raw[i] != '"'; ++i) {
      if (raw[i] == '\\') {
        if (++i == raw.size()) return invalid();
      }
    }
    if (i == raw.size()) return invalid();
    std::size_t next = i + 1;
    while (next < raw.size() && whitespace(raw[next]))
      ++next;
    if (next == raw.size() || raw[next] != ':') continue;
    // Every legal key is short; refuse oversized/unknown key tokens before
    // constructing even a temporary key DOM. Escaped key spelling is allowed.
    if (i - start > 128) return invalid();
    const auto key =
        Json::parse(raw.substr(start, i - start + 1), nullptr, false);
    if (!key.is_string()) return invalid();
    if (key.get_ref<const std::string&>() != "body_b64") continue;
    if (found_body) return invalid();
    found_body = true;
    ++next;
    while (next < raw.size() && whitespace(raw[next]))
      ++next;
    if (next == raw.size() || raw[next] != '"') return invalid();
    const auto body_start = ++next;
    while (next < raw.size() && raw[next] != '"') {
      if (raw[next] == '\\') return invalid();
      ++next;
    }
    if (next == raw.size()) return invalid();
    body_bytes = next - body_start;
    if (body_bytes > MAX_BODY64) return too_large();
    i = next;
  }
  if (raw.size() - body_bytes > MAX_ENCODED_METADATA_BYTES) return too_large();
  return {};
}
auto read_json(std::string_view raw, Role role) -> CodecResult<Json> {
  if (auto bounds = encoded_bounds(raw); !bounds)
    return std::unexpected(bounds.error());
  Preflight preflight{role};
  if (!Json::sax_parse(raw.begin(), raw.end(), &preflight))
    return std::unexpected(preflight.error);
  auto json = Json::parse(raw, nullptr, false);
  if (!json.is_object()) return invalid();
  return json;
}
auto schema_one(const Json& json) -> bool {
  return json.contains("schema") && json["schema"].is_number_integer() &&
         json["schema"] == 1;
}
auto strings(const Json& json, std::initializer_list<std::string_view> fields)
    -> bool {
  return std::ranges::all_of(fields, [&](auto field) {
    return json.contains(field) && json[field].is_string();
  });
}
auto read_headers(const Json& json) -> CodecResult<Headers> {
  if (!json.is_array()) return invalid();
  if (json.size() > MAX_HEADER_PAIRS) return too_large();
  Headers headers;
  headers.reserve(json.size());
  for (const auto& pair : json) {
    if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() ||
        !pair[1].is_string())
      return invalid();
    headers.emplace_back(pair[0].get<std::string>(),
                         pair[1].get<std::string>());
  }
  if (auto valid = validate_headers(headers); !valid)
    return std::unexpected(valid.error());
  return headers;
}
auto write_headers(const Headers& headers) -> Json {
  auto result = Json::array();
  for (const auto& [name, value] : headers)
    result.push_back(Json::array({name, value}));
  return result;
}
auto write_json(const Json& json, Role role) -> CodecResult<std::string> {
  auto raw = json.dump();
  if (auto bounds = encoded_bounds(raw); !bounds)
    return std::unexpected(bounds.error());
  Preflight preflight{role};
  if (!Json::sax_parse(raw.begin(), raw.end(), &preflight))
    return std::unexpected(preflight.error);
  return raw;
}
} // namespace

auto valid_utf8(std::string_view text) -> bool {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) continue;
    unsigned int code = 0;
    std::size_t count = 0;
    unsigned int minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      code = first & 0x1fU;
      count = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      code = first & 0x0fU;
      count = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code = first & 0x07U;
      count = 3;
      minimum = 0x10000;
    } else
      return false;
    if (count > text.size() - i) return false;
    for (std::size_t j = 0; j < count; ++j) {
      const auto c = static_cast<unsigned char>(text[i++]);
      if ((c & 0xc0U) != 0x80U) return false;
      code = (code << 6U) | (c & 0x3fU);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}
auto valid_uuid(std::string_view text) -> bool {
  if (text.size() != 36) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return false;
    } else if (!lower_hex(text[i]))
      return false;
  }
  return true;
}
auto valid_hex256(std::string_view text) -> bool {
  return text.size() == 64 && std::ranges::all_of(text, lower_hex);
}
auto parse_ns(std::string_view text) -> CodecResult<std::uint64_t> {
  if (text.empty() || text.size() > 20 ||
      (text.size() > 1 && text.front() == '0') ||
      !std::ranges::all_of(text, digit))
    return invalid();
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    return invalid();
  return value;
}
auto valid_name(std::string_view text) -> bool {
  return text.size() >= 2 && text.size() <= 64 && lower_alpha(text.front()) &&
         std::ranges::all_of(text, [](char c) {
           return lower_alpha(c) || digit(c) || c == '-';
         });
}
auto valid_version(std::string_view text) -> bool {
  if (text.empty() || text.size() > 256) return false;
  std::size_t pos = 0;
  for (int field = 0; field < 3; ++field) {
    const auto start = pos;
    while (pos < text.size() && digit(text[pos]))
      ++pos;
    if (pos == start || (pos - start > 1 && text[start] == '0')) return false;
    if (field < 2 && (pos == text.size() || text[pos++] != '.')) return false;
  }
  for (char delimiter : {'-', '+'}) {
    if (pos == text.size() || text[pos] != delimiter) continue;
    ++pos;
    while (true) {
      const auto start = pos;
      bool numeric = true;
      while (pos < text.size() &&
             (alpha(text[pos]) || digit(text[pos]) || text[pos] == '-')) {
        numeric = numeric && digit(text[pos]);
        ++pos;
      }
      if (pos == start || (delimiter == '-' && numeric && pos - start > 1 &&
                           text[start] == '0'))
        return false;
      if (pos == text.size() || text[pos] != '.') break;
      ++pos;
    }
  }
  return pos == text.size();
}
auto valid_method(std::string_view text) -> bool {
  return !text.empty() && text.size() <= 32 && text.front() >= 'A' &&
         text.front() <= 'Z' && std::ranges::all_of(text, [](char c) {
           return token(c) && !lower_alpha(c);
         });
}
auto valid_claim(std::string_view text) -> bool {
  if (text.size() > 256) return false;
  if (text.ends_with("/*")) text.remove_suffix(2);
  return text.size() > 1 && text.front() == '/' && text.back() != '/' &&
         path_segments(text) && std::ranges::all_of(text, [](char c) {
           return c == '/' || unreserved(c);
         });
}
auto validate_target(std::string_view original, std::string_view path,
                     std::string_view query) -> CodecResult<void> {
  if (original.size() > MAX_TARGET_BYTES || path.size() > MAX_TARGET_BYTES ||
      query.size() > MAX_TARGET_BYTES)
    return too_large();
  if (!valid_utf8(original) || !valid_utf8(path) || !path_segments(path))
    return invalid();
  for (const char c : path) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f || c == '\\' || c == '#') return invalid();
  }
  std::size_t decoded = 0;
  for (std::size_t i = 0; i < original.size(); ++i) {
    auto c = static_cast<unsigned char>(original[i]);
    if (c <= 0x20 || c == 0x7f || c == '\\' || c == '#' || c == '?')
      return invalid();
    if (c == '%') {
      if (original.size() - i < 3 || hex(original[i + 1]) < 0 ||
          hex(original[i + 2]) < 0)
        return invalid();
      c = static_cast<unsigned char>(hex(original[i + 1]) * 16 +
                                     hex(original[i + 2]));
      if (c == '/' || c == '\\' || unreserved(static_cast<char>(c)))
        return invalid();
      i += 2;
    }
    if (decoded == path.size() ||
        c != static_cast<unsigned char>(path[decoded++]))
      return invalid();
  }
  if (decoded != path.size()) return invalid();
  for (std::size_t i = 0; i < query.size(); ++i) {
    const auto c = static_cast<unsigned char>(query[i]);
    if (c < 0x21 || c > 0x7e || c == '#') return invalid();
    if (c == '%') {
      if (query.size() - i < 3 || hex(query[i + 1]) < 0 ||
          hex(query[i + 2]) < 0)
        return invalid();
      i += 2;
    }
  }
  return {};
}
auto validate_headers(const Headers& headers) -> CodecResult<void> {
  if (headers.size() > MAX_HEADER_PAIRS) return too_large();
  std::size_t total = 0;
  for (const auto& [name, value] : headers) {
    if (name.empty() || !std::ranges::all_of(name, [](char c) {
          return token(c) && !(c >= 'A' && c <= 'Z');
        }))
      return invalid();
    if (name.size() > 128 || value.size() > 8192) return too_large();
    if (!valid_utf8(value) || std::ranges::any_of(value, [](char c) {
          const auto u = static_cast<unsigned char>(c);
          return (u < 0x20 && c != '\t') || u == 0x7f;
        }))
      return invalid();
    if (name.size() + value.size() > MAX_HEADER_BYTES - total)
      return too_large();
    total += name.size() + value.size();
  }
  return {};
}
auto filter_request_headers(const Headers& headers) -> CodecResult<Headers> {
  if (auto valid = validate_headers(headers); !valid)
    return std::unexpected(valid.error());
  std::set<std::string> nominated;
  for (const auto& [name, value] : headers) {
    if (name != "connection") continue;
    std::size_t start = 0;
    while (start <= value.size()) {
      auto end = value.find(',', start);
      if (end == std::string::npos) end = value.size();
      auto item = std::string_view{value}.substr(start, end - start);
      while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
        item.remove_prefix(1);
      while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
        item.remove_suffix(1);
      if (item.empty() || item.size() > 128 ||
          !std::ranges::all_of(item, token))
        return invalid();
      std::string lower{item};
      for (auto& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      nominated.insert(std::move(lower));
      if (end == value.size()) break;
      start = end + 1;
    }
  }
  Headers result;
  result.reserve(headers.size());
  for (const auto& header : headers)
    if (!request_forbidden(header.first) && !nominated.contains(header.first))
      result.push_back(header);
  return result;
}
auto validate_request(const NativeRequest& request) -> CodecResult<void> {
  const auto& data = request.data;
  if (request.body.size() > MAX_BODY_BYTES) return too_large();
  if (!valid_uuid(data.request_id) || !valid_method(data.method))
    return invalid();
  if (auto valid = validate_target(data.target.original_path, data.target.path,
                                   data.target.query);
      !valid)
    return valid;
  if (auto valid = validate_headers(data.headers); !valid) return valid;
  for (const auto& [name, value] : data.headers) {
    static_cast<void>(value);
    if (request_forbidden(name)) return invalid();
  }
  if (data.request_id.size() + data.method.size() + data.target.path.size() +
          data.target.original_path.size() + data.target.query.size() +
          header_bytes(data.headers) >
      MAX_METADATA_BYTES)
    return too_large();
  return {};
}
auto validate_response(const NativeResponse& response) -> CodecResult<void> {
  if (response.status < 200 || response.status > 599) return invalid();
  if (response.body.size() > MAX_BODY_BYTES) return too_large();
  if (auto valid = validate_headers(response.headers); !valid) return valid;
  for (const auto& [name, value] : response.headers) {
    static_cast<void>(value);
    if (response_forbidden(name)) return invalid();
  }
  return {};
}
auto validate_context(const ContextData& context) -> CodecResult<void> {
  const auto& ext = context.extension;
  const auto& principal = context.principal;
  if (!valid_uuid(context.dispatch_id) || !valid_uuid(context.owner_epoch) ||
      !valid_uuid(context.adapter_epoch) ||
      !valid_uuid(context.clock_domain_id) ||
      context.admitted_at_ns > context.execution_cutoff_ns ||
      !valid_claim(context.matched_prefix) || !valid_uuid(ext.package_id) ||
      !valid_name(ext.name) || !valid_version(ext.version) ||
      !valid_hex256(ext.artifact_sha256) ||
      !valid_uuid(ext.generation_receipt_id))
    return invalid();
  if (principal.kind == Principal::Kind::ANONYMOUS) {
    if (!principal.user_id.empty() || !principal.pat_id.empty())
      return invalid();
  } else if (principal.kind != Principal::Kind::PAT ||
             !valid_uuid(principal.user_id) || !valid_uuid(principal.pat_id))
    return invalid();
  return {}; // Closed scalar bounds keep this record well below 64KiB.
}
auto validate_call_data(const NativeRequest& request,
                        const ContextData& context) -> CodecResult<void> {
  if (auto valid = validate_request(request); !valid) return valid;
  if (auto valid = validate_context(context); !valid) return valid;
  if (request.data.request_id != context.dispatch_id ||
      !matches(context.matched_prefix, request.data.target.path))
    return invalid();
  return {};
}
auto encode_body64(const Binary& body) -> CodecResult<std::string> {
  if (body.size() > MAX_BODY_BYTES) return too_large();
  std::string out;
  out.reserve(((body.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < body.size(); i += 3) {
    const auto remaining = body.size() - i;
    const auto a = static_cast<unsigned int>(body[i]);
    const auto b = remaining > 1 ? static_cast<unsigned int>(body[i + 1]) : 0U;
    const auto c = remaining > 2 ? static_cast<unsigned int>(body[i + 2]) : 0U;
    out.push_back(BASE64[a >> 2U]);
    out.push_back(BASE64[((a & 3U) << 4U) | (b >> 4U)]);
    out.push_back(remaining > 1 ? BASE64[((b & 15U) << 2U) | (c >> 6U)] : '=');
    out.push_back(remaining > 2 ? BASE64[c & 63U] : '=');
  }
  return out;
}
auto decode_body64(std::string_view text) -> CodecResult<Binary> {
  if (text.size() > MAX_BODY64) return too_large();
  if (text.size() % 4 != 0) return invalid();
  const auto padding = text.ends_with("==")  ? 2U
                       : text.ends_with("=") ? 1U
                                             : 0U;
  const auto size = text.size() / 4 * 3 - padding;
  if (size > MAX_BODY_BYTES) return too_large();
  Binary out;
  out.reserve(size);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    std::array<unsigned int, 4> quad{};
    for (std::size_t j = 0; j < 4; ++j) {
      if (text[i + j] == '=' && i + 4 == text.size() && j >= 4 - padding)
        continue;
      const auto pos = BASE64.find(text[i + j]);
      if (pos == std::string_view::npos) return invalid();
      quad[j] = static_cast<unsigned int>(pos);
    }
    if ((padding == 2 && i + 4 == text.size() && (quad[1] & 15U) != 0) ||
        (padding == 1 && i + 4 == text.size() && (quad[2] & 3U) != 0))
      return invalid();
    out.push_back(static_cast<std::uint8_t>((quad[0] << 2U) | (quad[1] >> 4U)));
    if (out.size() < size)
      out.push_back(
          static_cast<std::uint8_t>((quad[1] << 4U) | (quad[2] >> 2U)));
    if (out.size() < size)
      out.push_back(static_cast<std::uint8_t>((quad[2] << 6U) | quad[3]));
  }
  return out;
}
auto parse_wire_request(std::string_view raw) -> CodecResult<NativeRequest> {
  auto json = read_json(raw, Role::REQUEST);
  if (!json) return std::unexpected(json.error());
  const auto& j = *json;
  if (j.size() != 8 || !schema_one(j) ||
      !strings(j, {"request_id", "method", "path", "original_path", "query",
                   "body_b64"}) ||
      !j.contains("headers"))
    return invalid();
  auto headers = read_headers(j["headers"]);
  if (!headers) return std::unexpected(headers.error());
  auto body = decode_body64(j["body_b64"].get_ref<const std::string&>());
  if (!body) return std::unexpected(body.error());
  NativeRequest request{
      {j["request_id"].get<std::string>(),
       j["method"].get<std::string>(),
       {j["original_path"].get<std::string>(), j["path"].get<std::string>(),
        j["query"].get<std::string>()},
       std::move(*headers)},
      std::move(*body)};
  if (auto valid = validate_request(request); !valid)
    return std::unexpected(valid.error());
  return request;
}
auto parse_wire_response(std::string_view raw) -> CodecResult<NativeResponse> {
  auto json = read_json(raw, Role::RESPONSE);
  if (!json) return std::unexpected(json.error());
  const auto& j = *json;
  if (j.size() != 4 || !schema_one(j) || !strings(j, {"body_b64"}) ||
      !j.contains("headers") || !j.contains("status") ||
      !j["status"].is_number_integer() || j["status"] < 200 ||
      j["status"] > 599)
    return invalid();
  auto headers = read_headers(j["headers"]);
  if (!headers) return std::unexpected(headers.error());
  auto body = decode_body64(j["body_b64"].get_ref<const std::string&>());
  if (!body) return std::unexpected(body.error());
  NativeResponse response{j["status"].get<int>(), std::move(*headers),
                          std::move(*body)};
  if (auto valid = validate_response(response); !valid)
    return std::unexpected(valid.error());
  return response;
}
auto parse_context(std::string_view raw) -> CodecResult<ContextData> {
  auto json = read_json(raw, Role::CONTEXT);
  if (!json) return std::unexpected(json.error());
  const auto& j = *json;
  if (j.size() != 10 || !schema_one(j) ||
      !strings(j, {"dispatch_id", "owner_epoch", "adapter_epoch",
                   "clock_domain_id", "admitted_at_ns", "execution_cutoff_ns",
                   "matched_prefix"}) ||
      !j.contains("principal") || !j["principal"].is_object() ||
      !j.contains("extension") || !j["extension"].is_object())
    return invalid();
  const auto& p = j["principal"];
  const auto& e = j["extension"];
  Principal principal;
  if (!strings(p, {"kind"})) return invalid();
  if (p["kind"] == "anonymous") {
    if (p.size() != 1) return invalid();
  } else if (p["kind"] == "pat") {
    if (p.size() != 3 || !strings(p, {"user_id", "pat_id"})) return invalid();
    principal = {Principal::Kind::PAT, p["user_id"].get<std::string>(),
                 p["pat_id"].get<std::string>()};
  } else
    return invalid();
  if (e.size() != 5 ||
      !strings(e, {"package_id", "name", "version", "artifact_sha256",
                   "generation_receipt_id"}))
    return invalid();
  auto admitted = parse_ns(j["admitted_at_ns"].get_ref<const std::string&>());
  auto cutoff =
      parse_ns(j["execution_cutoff_ns"].get_ref<const std::string&>());
  if (!admitted || !cutoff) return invalid();
  ContextData context{
      j["dispatch_id"].get<std::string>(),
      j["owner_epoch"].get<std::string>(),
      j["adapter_epoch"].get<std::string>(),
      j["clock_domain_id"].get<std::string>(),
      *admitted,
      *cutoff,
      j["matched_prefix"].get<std::string>(),
      std::move(principal),
      {e["package_id"].get<std::string>(), e["name"].get<std::string>(),
       e["version"].get<std::string>(), e["artifact_sha256"].get<std::string>(),
       e["generation_receipt_id"].get<std::string>()}};
  if (auto valid = validate_context(context); !valid)
    return std::unexpected(valid.error());
  return context;
}
auto serialize_wire_request(const NativeRequest& request)
    -> CodecResult<std::string> {
  if (auto valid = validate_request(request); !valid)
    return std::unexpected(valid.error());
  auto body = encode_body64(request.body);
  if (!body) return std::unexpected(body.error());
  const auto& r = request.data;
  return write_json(Json{{"schema", 1},
                         {"request_id", r.request_id},
                         {"method", r.method},
                         {"path", r.target.path},
                         {"original_path", r.target.original_path},
                         {"query", r.target.query},
                         {"headers", write_headers(r.headers)},
                         {"body_b64", std::move(*body)}},
                    Role::REQUEST);
}
auto serialize_wire_response(const NativeResponse& response)
    -> CodecResult<std::string> {
  if (auto valid = validate_response(response); !valid)
    return std::unexpected(valid.error());
  auto body = encode_body64(response.body);
  if (!body) return std::unexpected(body.error());
  return write_json(Json{{"schema", 1},
                         {"status", response.status},
                         {"headers", write_headers(response.headers)},
                         {"body_b64", std::move(*body)}},
                    Role::RESPONSE);
}
auto serialize_context(const ContextData& context) -> CodecResult<std::string> {
  if (auto valid = validate_context(context); !valid)
    return std::unexpected(valid.error());
  const auto& e = context.extension;
  Json principal{{"kind", "anonymous"}};
  if (context.principal.kind == Principal::Kind::PAT)
    principal = Json{{"kind", "pat"},
                     {"user_id", context.principal.user_id},
                     {"pat_id", context.principal.pat_id}};
  return write_json(
      Json{{"schema", 1},
           {"dispatch_id", context.dispatch_id},
           {"owner_epoch", context.owner_epoch},
           {"adapter_epoch", context.adapter_epoch},
           {"clock_domain_id", context.clock_domain_id},
           {"admitted_at_ns", std::to_string(context.admitted_at_ns)},
           {"execution_cutoff_ns", std::to_string(context.execution_cutoff_ns)},
           {"matched_prefix", context.matched_prefix},
           {"principal", std::move(principal)},
           {"extension",
            Json{{"package_id", e.package_id},
                 {"name", e.name},
                 {"version", e.version},
                 {"artifact_sha256", e.artifact_sha256},
                 {"generation_receipt_id", e.generation_receipt_id}}}},
      Role::CONTEXT);
}
} // namespace plinth::extension_http
