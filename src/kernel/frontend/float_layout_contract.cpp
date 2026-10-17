#include "kernel/frontend/float_layout_contract.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"
#include "kernel/packages/detail/restore_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::frontend::layout_contract {
namespace {
using LayoutJson = nlohmann::json;
constexpr std::uint64_t MAX_SAFE_INTEGER = 9007199254740991ULL;

auto exact(const LayoutJson& object,
           std::initializer_list<std::string_view> keys) -> bool {
  return object.is_object() && object.size() == keys.size() &&
         std::ranges::all_of(keys,
                             [&](auto key) { return object.contains(key); });
}
auto identifier(std::string_view value, bool application) -> bool {
  return value.size() >= (application ? 2U : 1U) && value.size() <= 64 &&
         value.front() >= 'a' && value.front() <= 'z' &&
         std::ranges::all_of(value, [application](char byte) {
           return (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') || byte == '-' ||
                  (!application && byte == '_');
         });
}
auto text(std::string_view value, std::size_t minimum, std::size_t maximum)
    -> bool {
  return value.size() >= minimum && value.size() <= maximum &&
         value.find('\0') == std::string_view::npos &&
         packages::detail::unicode_scalar_count(value).has_value();
}
auto integer(const LayoutJson& value, std::uint64_t minimum,
             std::uint64_t maximum) -> std::optional<std::uint64_t> {
  // Check BEFORE conversion; no integer is rounded through double. The only
  // accepted float conversion range here is <=JS safe-integer maximum.
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number >= minimum && number <= maximum) return number;
  } else if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number >= 0 && static_cast<std::uint64_t>(number) >= minimum &&
        static_cast<std::uint64_t>(number) <= maximum)
      return static_cast<std::uint64_t>(number);
  } else if (value.is_number_float()) {
    const auto number = value.get<double>();
    if (std::isfinite(number) && std::trunc(number) == number &&
        number >= static_cast<double>(minimum) &&
        number <= static_cast<double>(maximum))
      return static_cast<std::uint64_t>(number);
  }
  return std::nullopt;
}
auto digest(std::string_view value) -> bool {
  return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}
auto uuid(std::string_view value) -> bool {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!((value[index] >= '0' && value[index] <= '9') ||
                 (value[index] >= 'a' && value[index] <= 'f')))
      return false;
  }
  return true;
}
auto positive_uint64(std::string_view value) -> bool {
  if (value.empty() || value.size() > 20 || value.front() == '0') return false;
  std::uint64_t number = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), number);
  return parsed.ec == std::errc{} &&
         parsed.ptr == value.data() + value.size() && number != 0;
}
auto canonical(const LayoutJson& value, std::size_t cap)
    -> std::expected<std::string, Error> {
  const auto result = packages::restore::detail::canonical_json(value, cap);
  if (!result)
    return std::unexpected(detail::classify_canonical_error(result.error()));
  return *result;
}

auto normalize_entry(const LayoutJson& value)
    -> std::expected<LayoutJson, Error> {
  if (!exact(value, {"application_id", "panel_id", "capability", "context",
                     "presentation", "maximized", "geometry"}) ||
      !value["application_id"].is_string() ||
      !identifier(value["application_id"].get_ref<const std::string&>(),
                  true) ||
      !value["panel_id"].is_string() ||
      !identifier(value["panel_id"].get_ref<const std::string&>(), false) ||
      !value["capability"].is_string() ||
      !text(value["capability"].get_ref<const std::string&>(), 1, 256) ||
      (value["presentation"] != "shown" &&
       value["presentation"] != "minimized") ||
      !value["maximized"].is_boolean() ||
      !exact(value["geometry"], {"x", "y", "width", "height"}) ||
      !value["context"].is_object() || value["context"].size() > 16)
    return std::unexpected(Error::INVALID);
  const auto parsed = capabilities::parse_signature(
      value["capability"].get_ref<const std::string&>());
  const auto* parts = std::get_if<capabilities::ParsedSignature>(&parsed);
  if (!parts) return std::unexpected(Error::INVALID);
  for (const auto& [key, child] : value["context"].items()) {
    if (!text(key, 1, 64) || key == "__proto__" || key == "prototype" ||
        key == "constructor" || !child.is_string() ||
        !text(child.get_ref<const std::string&>(), 0, 512))
      return std::unexpected(Error::INVALID);
  }
  const auto context = canonical(value["context"], 4096);
  if (!context) return std::unexpected(context.error());
  LayoutJson geometry = LayoutJson::object();
  for (const auto key : {"x", "y", "width", "height"}) {
    const auto coordinate = integer(
        value["geometry"][key],
        (std::string_view{key} == "x" || std::string_view{key} == "y") ? 0 : 1,
        65535);
    if (!coordinate) return std::unexpected(Error::INVALID);
    geometry[key] = *coordinate;
  }
  // Count the ORIGINAL capability string before canonical normalization, just
  // as browser savedEntry's dataJSON does. Numeric values are normalized by JS
  // JSON.stringify itself; no arbitrary floats remain after the safe checks.
  LayoutJson result{{"application_id", value["application_id"]},
                    {"panel_id", value["panel_id"]},
                    {"capability", value["capability"]},
                    {"context", LayoutJson::parse(*context)},
                    {"presentation", value["presentation"]},
                    {"maximized", value["maximized"]},
                    {"geometry", geometry}};
  const auto encoded = canonical(result, 6144);
  if (!encoded) return std::unexpected(encoded.error());
  result["capability"] = parts->namespace_ + ":" +
                         std::to_string(parts->version) + ":" + parts->function;
  return result;
}

auto identity(const LayoutJson& normalized)
    -> std::expected<std::string, Error> {
  const auto context = canonical(normalized["context"], 4096);
  if (!context) return std::unexpected(context.error());
  return canonical(
      LayoutJson::array({normalized["application_id"], normalized["panel_id"],
                         normalized["capability"], *context}),
      16384);
}

auto binding(const LayoutJson& value, std::size_t count)
    -> std::expected<FloatCommitBinding, Error> {
  if (!exact(value, {"index", "generation", "artifact_digest", "schema_digest",
                     "projection_contract", "policy_revision"}) ||
      !value["generation"].is_string() ||
      !uuid(value["generation"].get_ref<const std::string&>()) ||
      !value["artifact_digest"].is_string() ||
      !digest(value["artifact_digest"].get_ref<const std::string&>()) ||
      !value["schema_digest"].is_string() ||
      !digest(value["schema_digest"].get_ref<const std::string&>()) ||
      !value["policy_revision"].is_string() ||
      !positive_uint64(
          value["policy_revision"].get_ref<const std::string&>()) ||
      !exact(value["projection_contract"], {"id", "version"}) ||
      !value["projection_contract"]["id"].is_string() ||
      !identifier(
          value["projection_contract"]["id"].get_ref<const std::string&>(),
          false) ||
      count == 0)
    return std::unexpected(Error::INVALID);
  const auto index = integer(value["index"], 0, count - 1);
  const auto version =
      integer(value["projection_contract"]["version"], 1, MAX_SAFE_INTEGER);
  if (!index || !version) return std::unexpected(Error::INVALID);
  return FloatCommitBinding{
      .index = static_cast<std::size_t>(*index),
      .generation = value["generation"].get<std::string>(),
      .artifact_digest = value["artifact_digest"].get<std::string>(),
      .schema_digest = value["schema_digest"].get<std::string>(),
      .projection_contract =
          {.id = value["projection_contract"]["id"].get<std::string>(),
           .version = *version},
      .policy_revision = value["policy_revision"].get<std::string>()};
}

class CommitSax final : public nlohmann::json_sax<LayoutJson> {
 public:
  auto null() -> bool override { return value(); }
  auto boolean(bool) -> bool override { return value(); }
  auto number_integer(number_integer_t) -> bool override { return value(); }
  auto number_unsigned(number_unsigned_t) -> bool override { return value(); }
  auto number_float(number_float_t, const string_t&) -> bool override {
    return value();
  }
  auto string(string_t&) -> bool override { return value(); }
  auto binary(binary_t&) -> bool override { return false; }
  auto start_object(std::size_t) -> bool override { return begin(true); }
  auto start_array(std::size_t) -> bool override { return begin(false); }
  auto key(string_t& key) -> bool override {
    return !frames.empty() && frames.back().object &&
           frames.back().keys.insert(key).second;
  }
  auto end_object() -> bool override { return end(true); }
  auto end_array() -> bool override { return end(false); }
  auto parse_error(std::size_t, const std::string&,
                   const LayoutJson::exception&) -> bool override {
    return false;
  }
  [[nodiscard]] auto limited() const -> bool { return exceeded; }

 private:
  struct Frame {
    bool object;
    std::set<std::string> keys;
  };
  auto value() -> bool {
    if (frames.size() >= 6 || ++nodes > 16384) {
      exceeded = true;
      return false;
    }
    return true;
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
  std::size_t nodes = 0;
  bool exceeded = false;
  std::vector<Frame> frames;
};
} // namespace

auto parse_float_commit(std::string_view raw)
    -> std::expected<ParsedFloatCommit, Error> try {
  if (raw.size() > 65536) return std::unexpected(Error::TOO_LARGE);
  if (raw.starts_with("\xef\xbb\xbf")) return std::unexpected(Error::INVALID);
  CommitSax validator;
  if (!LayoutJson::sax_parse(raw.begin(), raw.end(), &validator))
    return std::unexpected(validator.limited() ? Error::STRUCTURAL_LIMIT
                                               : Error::INVALID);
  const auto root = LayoutJson::parse(raw.begin(), raw.end());
  if (!exact(root, {"schema_version", "layout", "bindings"}) ||
      !integer(root["schema_version"], 1, 1) ||
      !exact(root["layout"], {"version", "floats"}) ||
      !integer(root["layout"]["version"], 1, 1) ||
      !root["layout"]["floats"].is_array() ||
      root["layout"]["floats"].size() > 5 || !root["bindings"].is_array() ||
      root["bindings"].size() != root["layout"]["floats"].size())
    return std::unexpected(Error::INVALID);
  ParsedFloatCommit result{
      .layout = {{"version", 1}, {"floats", LayoutJson::array()}},
      .bindings = {}};
  LayoutJson original = LayoutJson::array();
  std::set<std::string> identities;
  for (const auto& raw_entry : root["layout"]["floats"]) {
    auto normalized = normalize_entry(raw_entry);
    if (!normalized) return std::unexpected(normalized.error());
    const auto key = identity(*normalized);
    if (!key) return std::unexpected(key.error());
    if (!identities.insert(*key).second) return std::unexpected(Error::INVALID);
    auto counted = *normalized;
    counted["capability"] = raw_entry["capability"];
    original.push_back(std::move(counted));
    result.layout["floats"].push_back(std::move(*normalized));
  }
  const auto original_encoded =
      canonical(LayoutJson{{"version", 1}, {"floats", original}}, 32768);
  if (!original_encoded) return std::unexpected(original_encoded.error());
  const auto normalized_encoded = canonical(result.layout, 32768);
  if (!normalized_encoded) return std::unexpected(normalized_encoded.error());
  std::set<std::size_t> indexes;
  for (const auto& row : root["bindings"]) {
    auto decoded = binding(row, root["bindings"].size());
    if (!decoded) return std::unexpected(decoded.error());
    if (!indexes.insert(decoded->index).second)
      return std::unexpected(Error::INVALID);
    result.bindings.push_back(std::move(*decoded));
  }
  std::ranges::sort(result.bindings, {}, &FloatCommitBinding::index);
  return result;
} catch (const LayoutJson::exception&) {
  return std::unexpected(Error::INVALID);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

auto validate_saved_float_entry(const LayoutJson& value) -> bool {
  return normalize_saved_float_entry(value).has_value();
}
auto normalize_saved_float_entry(const LayoutJson& value)
    -> std::expected<LayoutJson, Error> try {
  return normalize_entry(value);
} catch (const LayoutJson::exception&) {
  return std::unexpected(Error::INVALID);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}
auto saved_float_identity(const LayoutJson& value)
    -> std::expected<std::string, Error> try {
  const auto normalized = normalize_entry(value);
  if (!normalized) return std::unexpected(normalized.error());
  return identity(*normalized);
} catch (const LayoutJson::exception&) {
  return std::unexpected(Error::INVALID);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(Error::LIMIT_EXCEEDED);
}

} // namespace plinth::frontend::layout_contract
