#include "kernel/frontend/typed_requests.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <set>
#include <tuple>
#include <utility>
#include <variant>

namespace plinth::frontend {
namespace {

using Json = nlohmann::json;

// This deliberately bounds pre-DOM work rather than inspecting an already
// parsed object whose duplicate keys have been erased. Raw bytes are separately
// capped before this first pass. Frames/sets own every decoded key.
class RequestSax final : public nlohmann::json_sax<Json> {
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
  auto key(string_t& name) -> bool override {
    return !frames.empty() && frames.back().object &&
           frames.back().keys.insert(name).second;
  }
  auto end_object() -> bool override { return end(true); }
  auto end_array() -> bool override { return end(false); }
  auto parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) -> bool override {
    return false;
  }

 private:
  struct Frame {
    bool object;
    std::set<std::string> keys;
  };
  std::vector<Frame> frames;
  std::size_t nodes = 0;
  auto value() -> bool { return frames.size() < 6 && ++nodes <= 16384; }
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

auto exact(const Json& value, std::initializer_list<std::string_view> keys)
    -> bool {
  return value.is_object() && value.size() == keys.size() &&
         std::ranges::all_of(keys,
                             [&](auto key) { return value.contains(key); });
}

auto identifier(const Json& value, bool application) -> bool {
  if (!value.is_string()) return false;
  const auto& text = value.get_ref<const std::string&>();
  if (text.size() < (application ? 2U : 1U) || text.size() > 64 ||
      text.front() < 'a' || text.front() > 'z')
    return false;
  return std::ranges::all_of(text, [application](char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '-' || (!application && byte == '_');
  });
}

auto kind_of(const Json& value) -> std::optional<SurfaceKind> {
  if (value == "float") return SurfaceKind::FLOAT;
  if (value == "primary") return SurfaceKind::PRIMARY;
  if (value == "tray") return SurfaceKind::TRAY;
  return std::nullopt;
}

auto lookup(const Json& input) -> std::optional<SurfaceLookup> {
  if (!input.is_object() || !input.contains("application_id") ||
      !input.contains("panel_id") || !input.contains("kind") ||
      !input.contains("expected_generation") ||
      !identifier(input["application_id"], true) ||
      !identifier(input["panel_id"], false))
    return std::nullopt;
  const auto kind = kind_of(input["kind"]);
  if (!kind) return std::nullopt;
  const auto& expected = input["expected_generation"];
  const bool fresh = expected.is_null();
  if (!fresh && (!expected.is_string() ||
                 expected.get_ref<const std::string&>().empty() ||
                 expected.get_ref<const std::string&>().size() > 256))
    return std::nullopt;
  if (*kind == SurfaceKind::FLOAT) {
    if (fresh) {
      if (!exact(input, {"application_id", "panel_id", "kind",
                         "expected_generation", "content_type"}) ||
          !input["content_type"].is_string() ||
          !is_canonical_content_type(
              input["content_type"].get_ref<const std::string&>()))
        return std::nullopt;
    } else {
      if (!exact(input, {"application_id", "panel_id", "kind",
                         "expected_generation", "capability"}) ||
          !input["capability"].is_string())
        return std::nullopt;
      const auto& signature = input["capability"].get_ref<const std::string&>();
      if (signature.size() > 256) return std::nullopt;
      const auto parsed = capabilities::parse_signature(signature);
      const auto* parts = std::get_if<capabilities::ParsedSignature>(&parsed);
      if (!parts || signature != parts->namespace_ + ":" +
                                     std::to_string(parts->version) + ":" +
                                     parts->function)
        return std::nullopt;
    }
  } else if (!exact(input, {"application_id", "panel_id", "kind",
                            "expected_generation"})) {
    return std::nullopt;
  }
  return SurfaceLookup{
      .application_id = input["application_id"].get<std::string>(),
      .panel_id = input["panel_id"].get<std::string>(),
      .kind = *kind,
      .expected_generation =
          fresh ? std::nullopt : std::optional{expected.get<std::string>()},
      .content_type =
          *kind == SurfaceKind::FLOAT && fresh
              ? std::optional{input["content_type"].get<std::string>()}
              : std::nullopt,
      .capability = *kind == SurfaceKind::FLOAT && !fresh
                        ? std::optional{input["capability"].get<std::string>()}
                        : std::nullopt};
}

} // namespace

auto is_canonical_content_type(std::string_view value) -> bool {
  if (value.empty() || value.size() > 128) return false;
  bool component_start = true;
  std::size_t separators = 0;
  for (const char byte : value) {
    const bool alphanumeric =
        (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
    if (byte == '/') {
      if (component_start || ++separators != 1) return false;
      component_start = true;
    } else {
      if (!alphanumeric &&
          (component_start || !std::string_view{"!#$&^_.+-"}.contains(byte)))
        return false;
      component_start = false;
    }
  }
  return separators == 1 && !component_start;
}

auto parse_surface_admission(std::string_view raw)
    -> std::expected<std::vector<SurfaceLookup>, TypedRequestError> {
  if (raw.size() > 32768) return std::unexpected(TypedRequestError::TOO_LARGE);
  return typed_request_detail::operational_boundary(
      [&]() -> std::expected<std::vector<SurfaceLookup>, TypedRequestError> {
        try {
          RequestSax preflight;
          if (!Json::sax_parse(raw.begin(), raw.end(), &preflight))
            return std::unexpected(TypedRequestError::INVALID);
          const auto root = Json::parse(raw.begin(), raw.end());
          if (!exact(root, {"schema_version", "targets"}) ||
              !root["schema_version"].is_number_integer() ||
              root["schema_version"] != 1 || !root["targets"].is_array() ||
              root["targets"].size() > 38)
            return std::unexpected(TypedRequestError::INVALID);
          std::set<std::tuple<std::string, std::string, SurfaceKind>> seen;
          std::vector<SurfaceLookup> result;
          result.reserve(root["targets"].size());
          for (const auto& item : root["targets"]) {
            auto parsed = lookup(item);
            if (!parsed || !seen.emplace(parsed->application_id,
                                         parsed->panel_id, parsed->kind)
                                .second)
              return std::unexpected(TypedRequestError::INVALID);
            result.push_back(std::move(*parsed));
          }
          return result;
        } catch (const Json::exception&) {
          return std::unexpected(TypedRequestError::INVALID);
        }
      });
}

} // namespace plinth::frontend
