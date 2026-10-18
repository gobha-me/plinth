#include "kernel/frontend/surface_protocol.hpp"

#include "kernel/browser_origin.hpp"
#include "kernel/capabilities/parser.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"
#include "kernel/packages/detail/restore_json.hpp"
#include "kernel/packages/restore_digest.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <system_error>
#include <variant>
#include <vector>

namespace plinth::frontend::protocol {
namespace {
using WireJson = nlohmann::json;
constexpr std::string_view ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view CURSOR_DOMAIN = "plinth.surface.cursor.v1";
constexpr std::string_view CREDENTIAL_DOMAIN = "plinth.surface.credential.v1";
constexpr std::size_t MAX_CURSOR = 4096;
constexpr std::uint64_t MAX_SAFE_INTEGER = 9007199254740991ULL;

auto identifier(std::string_view value, std::size_t minimum,
                bool underscore = false) -> bool {
  return value.size() >= minimum && value.size() <= 64 &&
         value.front() >= 'a' && value.front() <= 'z' &&
         std::ranges::all_of(value, [underscore](char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                  ch == '-' || (underscore && ch == '_');
         });
}
auto hex_digit(char ch) -> bool {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}
auto digest(std::string_view value) -> bool {
  return value.size() == 64 && std::ranges::all_of(value, hex_digit);
}
auto uuid(std::string_view value) -> bool {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!hex_digit(value[index]))
      return false;
  }
  return true;
}
auto positive_uint64(std::string_view text) -> bool {
  if (text.empty() || text.size() > 20 || text.front() == '0') return false;
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         value != 0;
}
auto metadata_text(std::string_view text, std::size_t bytes,
                   std::size_t scalars) -> bool {
  if (text.empty() || text.size() > bytes) return false;
  const auto count = packages::detail::unicode_scalar_count(text);
  return count && *count <= scalars;
}
auto profile(std::string_view value) -> bool {
  return value.size() == 104 && value.starts_with("v1:") &&
         uuid(value.substr(3, 36)) && value[39] == ':' &&
         digest(value.substr(40));
}
auto kind_name(SurfaceKind kind) -> std::string_view {
  switch (kind) {
    case SurfaceKind::FLOAT: return "float";
    case SurfaceKind::PRIMARY: return "primary";
    case SurfaceKind::TRAY: return "tray";
  }
  return {};
}
auto schema_error(packages::restore::CodecError error) -> Error {
  using enum packages::restore::CodecError;
  switch (error) {
    case TOO_LARGE:
    case LIMIT_EXCEEDED:
    case HASHING_FAILED: return Error::LIMIT_EXCEEDED;
    default: return Error::INVALID;
  }
}
auto position_valid(const authority::Position& value, SurfaceKind kind)
    -> bool {
  const bool rank_valid =
      kind == SurfaceKind::FLOAT
          ? value.logical_install_order &&
                positive_uint64(*value.logical_install_order)
          : !kind_name(kind).empty() && !value.logical_install_order;
  return rank_valid && identifier(value.application_id, 2) &&
         identifier(value.panel_id, 1, true);
}
auto query_valid(const CursorQuery& query) -> bool {
  return query.limit >= 1 && query.limit <= 64 &&
         !kind_name(query.kind).empty() && profile(query.platform_profile) &&
         (query.kind == SurfaceKind::FLOAT ||
          (!query.content_type && !query.include_admin_default)) &&
         (!query.content_type ||
          is_canonical_content_type(*query.content_type)) &&
         (!query.include_admin_default || query.content_type.has_value());
}
auto auth_valid(const auth::AuthContext& value) -> bool {
  return uuid(value.user_id) && digest(value.token_hash) &&
         ((value.auth_type == "session" &&
           (value.credential_source == auth::CredentialSource::COOKIE ||
            value.credential_source == auth::CredentialSource::BEARER)) ||
          (value.auth_type == "pat" &&
           value.credential_source == auth::CredentialSource::BEARER));
}
auto exact(const WireJson& object, std::initializer_list<std::string_view> keys)
    -> bool {
  return object.is_object() && object.size() == keys.size() &&
         std::ranges::all_of(keys,
                             [&](auto key) { return object.contains(key); });
}
auto b64(std::string_view input) -> std::string {
  std::string output;
  output.reserve((input.size() * 4 + 2) / 3);
  for (std::size_t index = 0; index < input.size(); index += 3) {
    const auto first = static_cast<unsigned char>(input[index]);
    const auto second = index + 1 < input.size()
                            ? static_cast<unsigned char>(input[index + 1])
                            : 0;
    const auto third = index + 2 < input.size()
                           ? static_cast<unsigned char>(input[index + 2])
                           : 0;
    output.push_back(ALPHABET[first >> 2]);
    output.push_back(ALPHABET[((first & 3U) << 4U) | (second >> 4U)]);
    if (index + 1 < input.size())
      output.push_back(ALPHABET[((second & 15U) << 2U) | (third >> 6U)]);
    if (index + 2 < input.size()) output.push_back(ALPHABET[third & 63U]);
  }
  return output;
}
auto unb64(std::string_view input) -> std::optional<std::string> {
  if (input.empty() || input.size() > MAX_CURSOR || input.size() % 4 == 1)
    return std::nullopt;
  std::string output;
  output.reserve(input.size() * 3 / 4);
  unsigned int bits = 0, count = 0;
  for (char ch : input) {
    const auto value = ALPHABET.find(ch);
    if (value == std::string_view::npos) return std::nullopt;
    bits = (bits << 6U) | static_cast<unsigned int>(value);
    count += 6;
    if (count >= 8) {
      count -= 8;
      output.push_back(static_cast<char>((bits >> count) & 255U));
    }
    bits &= (1U << count) - 1U;
  }
  if (bits != 0 || b64(output) != input) return std::nullopt;
  return output;
}
auto hmac(const std::array<unsigned char, 32>& secret, std::string_view domain,
          std::string_view bytes) -> std::expected<std::string, Error> {
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
      EVP_DigestSignUpdate(context.get(), bytes.data(), bytes.size()) != 1 ||
      EVP_DigestSignFinal(context.get(), result.data(), &size) != 1 ||
      size != result.size())
    return std::unexpected(Error::CRYPTO_FAILED);
  return std::string{reinterpret_cast<const char*>(result.data()),
                     result.size()};
}
auto fingerprint(const std::array<unsigned char, 32>& secret,
                 const auth::AuthContext& auth)
    -> std::expected<std::string, Error> {
  const WireJson value{
      {"auth_type", auth.auth_type},
      {"source", auth.credential_source == auth::CredentialSource::COOKIE
                     ? "cookie"
                     : "bearer"},
      {"token_hash", auth.token_hash}};
  const auto encoded = packages::restore::detail::canonical_json(value, 256);
  if (!encoded) return std::unexpected(Error::INVALID);
  const auto result = hmac(secret, CREDENTIAL_DOMAIN, *encoded);
  if (!result) return std::unexpected(result.error());
  return b64(*result);
}
auto unreserved(unsigned char byte) -> bool {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
         byte == '.' || byte == '~';
}
auto encode_component(std::string_view input) -> std::string {
  constexpr std::string_view HEX = "0123456789ABCDEF";
  std::string output;
  for (const unsigned char byte : input) {
    if (unreserved(byte))
      output.push_back(static_cast<char>(byte));
    else {
      output.push_back('%');
      output.push_back(HEX[byte >> 4U]);
      output.push_back(HEX[byte & 15U]);
    }
  }
  return output;
}
auto decode_component(std::string_view raw) -> std::optional<std::string> {
  std::string output;
  const auto hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < raw.size(); ++index) {
    if (raw[index] == '%') {
      if (index + 2 >= raw.size()) return std::nullopt;
      const int high = hex(raw[index + 1]), low = hex(raw[index + 2]);
      if (high < 0 || low < 0) return std::nullopt;
      output.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else
      output.push_back(raw[index]);
  }
  if (!metadata_text(output, 1024, 1024) || output == "." || output == ".." ||
      output.find_first_of(std::string_view{"/\\\0", 3}) != std::string::npos ||
      encode_component(output) != raw)
    return std::nullopt;
  return output;
}
auto path_components(std::string_view input) -> std::vector<std::string_view> {
  std::vector<std::string_view> result;
  for (std::size_t begin = 0; begin <= input.size();) {
    const auto end = input.find('/', begin);
    result.push_back(
        input.substr(begin, end == std::string_view::npos ? end : end - begin));
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return result;
}

auto common_target(const authority::SurfaceTarget& target,
                   const packages::generation::CapturedArtifact& artifact,
                   SurfaceKind expected, std::string_view trusted_origin)
    -> std::expected<WireJson, Error> {
  const auto& panel = target.declaration;
  const auto& key = artifact.key();
  const auto type = expected == SurfaceKind::PRIMARY
                        ? packages::PanelType::PRIMARY
                        : packages::PanelType::TRAY;
  if (target.kind != expected || !panel.typed_declaration ||
      panel.type != type || !identifier(target.application_id, 2) ||
      !identifier(panel.id, 1, true) || !uuid(target.generation) ||
      key.package_id != target.generation ||
      key.name != target.application_id || key.version != target.version ||
      key.profile != 1 || !digest(artifact.digest()) ||
      !metadata_text(target.version, 256, 256) ||
      target.logical_install_order || panel.capability ||
      !panel.content_types.empty() || trusted_origin.empty() ||
      trusted_origin.size() > 2048 || !valid_browser_origin(trusted_origin))
    return std::unexpected(Error::INVALID);
  const auto& title = panel.title ? *panel.title : panel.id;
  const std::string_view icon =
      panel.icon ? std::string_view{*panel.icon} : std::string_view{"square"};
  if (!metadata_text(title, 1024, 256) ||
      !packages::detail::is_icon_token_valid(icon) ||
      panel.client_path.empty() || panel.client_path.size() > 1024)
    return std::unexpected(Error::INVALID);
  std::string url = "/_plinth/typed/" + target.generation + "/" +
                    artifact.digest() + "/" + panel.id + "/client/panels";
  for (const auto part : path_components(panel.client_path)) {
    url.push_back('/');
    url += encode_component(part);
  }
  const auto selected = parse_asset_selector(url);
  if (!selected ||
      selected->member_path != "client/panels/" + panel.client_path ||
      url.size() > 2048 - trusted_origin.size())
    return std::unexpected(Error::INVALID);
  const auto* member = artifact.find(selected->member_path);
  if (!member || member->kind != packages::restore::ArtifactKind::REGULAR)
    return std::unexpected(Error::INVALID);
  return WireJson{{"application_id", target.application_id},
                  {"panel_id", panel.id},
                  {"type", kind_name(expected)},
                  {"generation", target.generation},
                  {"version", target.version},
                  {"title", title},
                  {"icon", icon},
                  {"module_url", std::move(url)}};
}

// No arbitrary DOM construction until raw byte/depth/node/decoded-duplicate
// limits pass. Cursor MAC validation occurs before even this bounded parser.
class CursorSax final : public nlohmann::json_sax<WireJson> {
 public:
  auto null() -> bool override { return value(); }
  auto boolean(bool) -> bool override { return value(); }
  auto number_integer(number_integer_t) -> bool override { return value(); }
  auto number_unsigned(number_unsigned_t) -> bool override { return value(); }
  auto number_float(number_float_t, const string_t&) -> bool override {
    return false;
  }
  auto string(string_t&) -> bool override { return value(); }
  auto binary(binary_t&) -> bool override { return false; }
  auto start_object(std::size_t) -> bool override {
    if (keys.size() == 6 || !value()) return false;
    keys.emplace_back();
    return true;
  }
  auto start_array(std::size_t) -> bool override { return false; }
  auto key(string_t& key) -> bool override {
    return !keys.empty() && keys.back().insert(key).second;
  }
  auto end_object() -> bool override {
    keys.pop_back();
    return true;
  }
  auto end_array() -> bool override { return false; }
  auto parse_error(std::size_t, const std::string&, const WireJson::exception&)
      -> bool override {
    return false;
  }

 private:
  auto value() -> bool { return ++nodes <= 128; }
  std::size_t nodes = 0;
  std::vector<std::set<std::string>> keys;
};
auto unsigned_seconds(const WireJson& value) -> std::optional<std::int64_t> {
  if (!value.is_number_integer()) return std::nullopt;
  if (value.is_number_unsigned()) {
    const auto seconds = value.get<std::uint64_t>();
    if (seconds >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return std::nullopt;
    return static_cast<std::int64_t>(seconds);
  }
  const auto seconds = value.get<std::int64_t>();
  return seconds >= 0 ? std::optional{seconds} : std::nullopt;
}
} // namespace

auto parse_asset_selector(std::string_view raw)
    -> std::expected<AssetSelector, Error> try {
  if (raw.size() > 2048) return std::unexpected(Error::LIMIT_EXCEEDED);
  if (raw.find_first_of("?#") != std::string_view::npos)
    return std::unexpected(Error::INVALID);
  const auto pieces = path_components(raw);
  if (pieces.size() < 7 || pieces[0] != "" || pieces[1] != "_plinth" ||
      (pieces[2] != "typed" && pieces[2] != "platform") || !uuid(pieces[3]) ||
      !digest(pieces[4]))
    return std::unexpected(Error::INVALID);
  const bool typed = pieces[2] == "typed";
  const std::size_t client = typed ? 6 : 5;
  if (pieces.size() <= client + 1 || pieces[client] != "client" ||
      (typed && !identifier(pieces[5], 1, true)))
    return std::unexpected(Error::INVALID);
  AssetSelector selector{
      typed ? AssetKind::TYPED : AssetKind::PLATFORM, std::string{pieces[3]},
      std::string{pieces[4]},
      typed ? std::optional{std::string{pieces[5]}} : std::nullopt, "client"};
  if (pieces.size() - client > packages::restore::INVENTORY_MAX_COMPONENTS)
    return std::unexpected(Error::LIMIT_EXCEEDED);
  for (std::size_t index = client + 1; index < pieces.size(); ++index) {
    const auto decoded = decode_component(pieces[index]);
    if (!decoded) return std::unexpected(Error::INVALID);
    if (selector.member_path.size() + 1 + decoded->size() >
        packages::restore::INVENTORY_PATH_MAX_BYTES)
      return std::unexpected(Error::LIMIT_EXCEEDED);
    selector.member_path.push_back('/');
    selector.member_path.append(*decoded);
  }
  return selector;
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

auto encode_float_target(const authority::SurfaceTarget& target,
                         const packages::generation::CapturedArtifact& artifact,
                         const std::optional<PersistenceProjection>& projection,
                         std::string_view trusted_origin)
    -> std::expected<WireJson, Error> try {
  const auto& panel = target.declaration;
  const auto& key = artifact.key();
  if (target.kind != SurfaceKind::FLOAT || !panel.typed_declaration ||
      panel.type != packages::PanelType::FLOAT ||
      !identifier(target.application_id, 2) || !identifier(panel.id, 1, true) ||
      !uuid(target.generation) || key.package_id != target.generation ||
      key.name != target.application_id || key.version != target.version ||
      key.profile != 1 || !digest(artifact.digest()) ||
      !metadata_text(target.version, 256, 256) || !panel.capability ||
      panel.capability->size() > 256 || !panel.context_schema ||
      !target.logical_install_order ||
      !positive_uint64(*target.logical_install_order) ||
      panel.content_types.size() > 32 || trusted_origin.empty() ||
      trusted_origin.size() > 2048 || !valid_browser_origin(trusted_origin))
    return std::unexpected(Error::INVALID);
  const auto parsed = capabilities::parse_signature(*panel.capability);
  const auto* signature = std::get_if<capabilities::ParsedSignature>(&parsed);
  if (!signature || signature->namespace_ + ":" +
                            std::to_string(signature->version) + ":" +
                            signature->function !=
                        *panel.capability)
    return std::unexpected(Error::INVALID);
  const auto& title = panel.title ? *panel.title : panel.id;
  const std::string_view icon =
      panel.icon ? std::string_view{*panel.icon} : std::string_view{"square"};
  if (!metadata_text(title, 1024, 256) ||
      !packages::detail::is_icon_token_valid(icon))
    return std::unexpected(Error::INVALID);
  std::set<std::string> types;
  for (const auto& type : panel.content_types)
    if (!is_canonical_content_type(type) || !types.insert(type).second)
      return std::unexpected(Error::INVALID);
  const auto schema =
      packages::restore::canonical_schema(*panel.context_schema);
  if (!schema) return std::unexpected(Error::INVALID);
  if (panel.client_path.empty() || panel.client_path.size() > 1024)
    return std::unexpected(Error::INVALID);
  std::string url = "/_plinth/typed/" + target.generation + "/" +
                    artifact.digest() + "/" + panel.id + "/client/panels";
  for (const auto part : path_components(panel.client_path)) {
    url.push_back('/');
    url += encode_component(part);
  }
  const auto selected = parse_asset_selector(url);
  if (!selected ||
      selected->member_path != "client/panels/" + panel.client_path ||
      url.size() > 2048 - trusted_origin.size())
    return std::unexpected(Error::INVALID);
  const auto* member = artifact.find(selected->member_path);
  if (!member || member->kind != packages::restore::ArtifactKind::REGULAR)
    return std::unexpected(Error::INVALID);
  WireJson persistence = nullptr;
  if (projection) {
    const auto& value = *projection;
    const auto computed =
        packages::restore::schema_digest(*panel.context_schema);
    if (!identifier(value.projection_contract.id, 1, true) ||
        value.projection_contract.version == 0 ||
        value.projection_contract.version > MAX_SAFE_INTEGER ||
        value.artifact_digest != artifact.digest() || !computed ||
        value.schema_digest != *computed ||
        !positive_uint64(value.policy_revision) ||
        value.approved_keys.size() > 16)
      return std::unexpected(Error::INVALID);
    std::set<std::string> approved;
    for (const auto& name : value.approved_keys)
      if (!panel.context_schema->fields.contains(name) ||
          std::ranges::find(panel.context_schema->persist_keys, name) ==
              panel.context_schema->persist_keys.end() ||
          !approved.insert(name).second)
        return std::unexpected(Error::INVALID);
    for (const auto& [name, field] : panel.context_schema->fields)
      if (field.required && !approved.contains(name))
        return std::unexpected(Error::INVALID);
    persistence = {{"projection_contract",
                    {{"id", value.projection_contract.id},
                     {"version", value.projection_contract.version}}},
                   {"artifact_digest", value.artifact_digest},
                   {"schema_digest", value.schema_digest},
                   {"approved_keys", value.approved_keys},
                   {"policy_revision", value.policy_revision}};
  }
  WireJson result{{"application_id", target.application_id},
                  {"panel_id", panel.id},
                  {"type", "float"},
                  {"generation", target.generation},
                  {"version", target.version},
                  {"title", title},
                  {"icon", icon},
                  {"module_url", url},
                  {"capability", *panel.capability},
                  {"content_types", panel.content_types},
                  {"context_schema", WireJson::parse(*schema)},
                  {"logical_install_order", *target.logical_install_order},
                  {"persistence_contract", persistence}};
  if (!packages::restore::detail::canonical_json(result, 16384))
    return std::unexpected(Error::LIMIT_EXCEEDED);
  return result;
} catch (const WireJson::exception&) {
  return std::unexpected(Error::INVALID);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

auto encode_primary_target(
    const authority::SurfaceTarget& target,
    const packages::generation::CapturedArtifact& artifact,
    std::string_view trusted_origin) -> std::expected<WireJson, Error> try {
  const auto& panel = target.declaration;
  if (!panel.accepts_navigation_intent || !panel.context_schema ||
      !panel.tray_states.empty() || panel.tray_initial_state ||
      panel.preferred_width || panel.chrome_essential)
    return std::unexpected(Error::INVALID);
  auto result =
      common_target(target, artifact, SurfaceKind::PRIMARY, trusted_origin);
  if (!result) return std::unexpected(result.error());
  const auto schema =
      packages::restore::canonical_schema(*panel.context_schema);
  if (!schema) return std::unexpected(schema_error(schema.error()));
  (*result)["accepts_navigation_intent"] = true;
  (*result)["context_schema"] = WireJson::parse(*schema);
  if (!packages::restore::detail::canonical_json(*result, 16384))
    return std::unexpected(Error::LIMIT_EXCEEDED);
  return result;
} catch (const WireJson::exception&) {
  return std::unexpected(Error::INVALID);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

auto encode_tray_target(const authority::SurfaceTarget& target,
                        const packages::generation::CapturedArtifact& artifact,
                        bool chrome_essential, std::string_view trusted_origin)
    -> std::expected<WireJson, Error> try {
  const auto& panel = target.declaration;
  if (panel.accepts_navigation_intent || panel.tray_states.empty() ||
      panel.tray_states.size() > 16 || !panel.tray_initial_state ||
      !panel.tray_states.contains(*panel.tray_initial_state) ||
      !panel.preferred_width || *panel.preferred_width < 280 ||
      *panel.preferred_width > 480)
    return std::unexpected(Error::INVALID);
  // A valid optional schema is inert for trays: validate it but never expose
  // primary/float-only wire keys or infer navigation/persistence authority.
  if (panel.context_schema) {
    const auto schema =
        packages::restore::canonical_schema(*panel.context_schema);
    if (!schema) return std::unexpected(schema_error(schema.error()));
  }
  for (const auto& [state, icon] : panel.tray_states)
    if (!packages::detail::is_icon_token_valid(state) ||
        !packages::detail::is_icon_token_valid(icon))
      return std::unexpected(Error::INVALID);
  auto result =
      common_target(target, artifact, SurfaceKind::TRAY, trusted_origin);
  if (!result) return std::unexpected(result.error());
  (*result)["tray_states"] = panel.tray_states;
  (*result)["tray_initial_state"] = *panel.tray_initial_state;
  (*result)["preferred_width"] = *panel.preferred_width;
  (*result)["chrome_essential"] = chrome_essential;
  if (!packages::restore::detail::canonical_json(*result, 16384))
    return std::unexpected(Error::LIMIT_EXCEEDED);
  return result;
} catch (const WireJson::exception&) {
  return std::unexpected(Error::INVALID);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

CursorCodec::CursorCodec() {
  if (RAND_bytes(secret.data(), static_cast<int>(secret.size())) != 1)
    throw std::runtime_error("surface cursor CSPRNG failed");
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
                         const CursorQuery& query,
                         const authority::Position& position,
                         std::int64_t now) const
    -> std::expected<std::string, Error> try {
  if (!auth_valid(auth) || !query_valid(query) ||
      !position_valid(position, query.kind) || now < 0 ||
      now > std::numeric_limits<std::int64_t>::max() - 60)
    return std::unexpected(Error::INVALID);
  const auto credential = fingerprint(secret, auth);
  if (!credential) return std::unexpected(credential.error());
  WireJson position_wire{{"application_id", position.application_id},
                         {"panel_id", position.panel_id}};
  if (position.logical_install_order)
    position_wire["logical_install_order"] = *position.logical_install_order;
  const WireJson payload{{"v", 1},
                         {"user", auth.user_id},
                         {"credential", *credential},
                         {"kind", kind_name(query.kind)},
                         {"content_type", query.content_type
                                              ? WireJson(*query.content_type)
                                              : WireJson(nullptr)},
                         {"include_admin_default", query.include_admin_default},
                         {"limit", query.limit},
                         {"platform_profile", query.platform_profile},
                         {"position", std::move(position_wire)},
                         {"issued", now},
                         {"expires", now + 60}};
  const auto bytes =
      packages::restore::detail::canonical_json(payload, MAX_CURSOR);
  if (!bytes) return std::unexpected(Error::LIMIT_EXCEEDED);
  const auto mac = hmac(secret, CURSOR_DOMAIN, *bytes);
  if (!mac) return std::unexpected(mac.error());
  auto raw = b64(*bytes) + "." + b64(*mac);
  if (raw.size() > MAX_CURSOR) return std::unexpected(Error::LIMIT_EXCEEDED);
  return raw;
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

auto CursorCodec::decode(std::string_view raw, const auth::AuthContext& auth,
                         const CursorQuery& query, std::int64_t now) const
    -> std::expected<authority::Position, Error> try {
  if (raw.size() > MAX_CURSOR) return std::unexpected(Error::LIMIT_EXCEEDED);
  if (!auth_valid(auth) || !query_valid(query) || now < 0)
    return std::unexpected(Error::INVALID);
  const auto dot = raw.find('.');
  if (dot == std::string_view::npos ||
      raw.find('.', dot + 1) != std::string_view::npos)
    return std::unexpected(Error::INVALID);
  const auto bytes = unb64(raw.substr(0, dot)),
             supplied = unb64(raw.substr(dot + 1));
  if (!bytes || !supplied || supplied->size() != 32)
    return std::unexpected(Error::INVALID);
  const auto actual = hmac(secret, CURSOR_DOMAIN, *bytes);
  if (!actual) return std::unexpected(actual.error());
  if (CRYPTO_memcmp(actual->data(), supplied->data(), actual->size()) != 0)
    return std::unexpected(Error::INVALID);
  CursorSax validator;
  if (!WireJson::sax_parse(*bytes, &validator))
    return std::unexpected(Error::INVALID);
  const auto payload = WireJson::parse(*bytes);
  const auto canonical =
      packages::restore::detail::canonical_json(payload, MAX_CURSOR);
  if (!canonical || *canonical != *bytes ||
      !exact(payload, {"v", "user", "credential", "kind", "content_type",
                       "include_admin_default", "limit", "platform_profile",
                       "position", "issued", "expires"}))
    return std::unexpected(Error::INVALID);
  const auto credential = fingerprint(secret, auth);
  if (!credential) return std::unexpected(credential.error());
  if (!payload["v"].is_number_integer() || payload["v"] != 1 ||
      payload["user"] != auth.user_id || !payload["credential"].is_string() ||
      payload["credential"].get_ref<const std::string&>().size() !=
          credential->size() ||
      CRYPTO_memcmp(payload["credential"].get_ref<const std::string&>().data(),
                    credential->data(), credential->size()) != 0 ||
      payload["kind"] != std::string{kind_name(query.kind)} ||
      payload["content_type"] != (query.content_type
                                      ? WireJson(*query.content_type)
                                      : WireJson(nullptr)) ||
      !payload["include_admin_default"].is_boolean() ||
      payload["include_admin_default"] != query.include_admin_default ||
      !payload["limit"].is_number_integer() ||
      payload["limit"] != query.limit ||
      payload["platform_profile"] != query.platform_profile ||
      !(query.kind == SurfaceKind::FLOAT
            ? exact(payload["position"],
                    {"logical_install_order", "application_id", "panel_id"})
            : exact(payload["position"], {"application_id", "panel_id"})))
    return std::unexpected(Error::INVALID);
  const auto issued = unsigned_seconds(payload["issued"]),
             expires = unsigned_seconds(payload["expires"]);
  if (!issued || !expires ||
      *issued > std::numeric_limits<std::int64_t>::max() - 60 ||
      *expires != *issued + 60 || now < *issued || now >= *expires)
    return std::unexpected(Error::INVALID);
  const auto& position = payload["position"];
  if ((query.kind == SurfaceKind::FLOAT &&
       !position["logical_install_order"].is_string()) ||
      !position["application_id"].is_string() ||
      !position["panel_id"].is_string())
    return std::unexpected(Error::INVALID);
  authority::Position result{
      query.kind == SurfaceKind::FLOAT
          ? std::optional{position["logical_install_order"].get<std::string>()}
          : std::nullopt,
      position["application_id"].get<std::string>(),
      position["panel_id"].get<std::string>()};
  if (!position_valid(result, query.kind))
    return std::unexpected(Error::INVALID);
  return result;
} catch (const WireJson::exception&) {
  return std::unexpected(Error::INVALID);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

} // namespace plinth::frontend::protocol
