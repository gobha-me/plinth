#include "kernel/packages/generation_binding.hpp"

#include "kernel/db/operations.hpp"
#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/detail/restore_json.hpp"
#include "kernel/packages/manifest.hpp"
#include "kernel/packages/migrations_internal.hpp"
#include "kernel/packages/panels.hpp"
#include "kernel/packages/panels_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace plinth::packages::generation {
namespace {
using Json = nlohmann::json;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
constexpr std::size_t MAX_ROWS = 4096;
constexpr std::size_t MAX_METADATA = std::size_t{1024} * 1024;
constexpr std::size_t WORKING_BYTES = std::size_t{16} * 1024 * 1024;
constexpr std::size_t MAX_DEPTH = 32;
constexpr std::size_t MAX_NODES = 65536;
constexpr std::string_view DOMAIN = "plinth.generation.binding.v1";

class ManifestPreflight final : public nlohmann::json_sax<Json> {
 public:
  explicit ManifestPreflight(std::size_t& total, std::size_t depth = MAX_DEPTH)
      : nodes(total), depth_limit(depth) {}
  auto null() -> bool override { return value(); }
  auto boolean(bool) -> bool override { return value(); }
  auto number_integer(number_integer_t) -> bool override { return value(); }
  auto number_unsigned(number_unsigned_t) -> bool override { return value(); }
  auto number_float(number_float_t, const string_t&) -> bool override {
    return value();
  }
  auto string(string_t&) -> bool override { return value(); }
  auto binary(binary_t&) -> bool override { return false; }
  auto start_object(std::size_t) -> bool override { return container(); }
  auto start_array(std::size_t) -> bool override { return container(); }
  auto key(string_t& key) -> bool override {
    return !keys.empty() && keys.back().insert(key).second;
  }
  auto end_object() -> bool override {
    keys.pop_back();
    return true;
  }
  auto end_array() -> bool override {
    keys.pop_back();
    return true;
  }
  auto parse_error(std::size_t, const std::string&, const Json::exception&)
      -> bool override {
    return false;
  }
  [[nodiscard]] auto exceeded() const -> bool { return limited; }

 private:
  auto value() -> bool {
    if (nodes == MAX_NODES) {
      limited = true;
      return false;
    }
    ++nodes;
    return true;
  }
  auto container() -> bool {
    if (keys.size() == depth_limit) {
      limited = true;
      return false;
    }
    if (!value()) return false;
    keys.emplace_back();
    return true;
  }
  std::size_t& nodes;
  std::size_t depth_limit;
  std::vector<std::set<std::string>> keys;
  bool limited = false;
};

auto valid_uuid(std::string_view id) -> bool {
  if (id.size() != 36) return false;
  for (std::size_t index = 0; index < id.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (id[index] != '-') return false;
    } else if (!((id[index] >= '0' && id[index] <= '9') ||
                 (id[index] >= 'a' && id[index] <= 'f')))
      return false;
  }
  return true;
}

auto valid_ordinal(const Json& ordinal) -> bool {
  if (!ordinal.is_string()) return false;
  const auto& text = ordinal.get_ref<const std::string&>();
  if (text.empty() || text.front() == '0') return false;
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         value != 0;
}

auto same_number(const Json& left, const Json& right) -> bool {
  if (left.is_number_float() && right.is_number_float())
    return left.get<double>() == right.get<double>();
  if (left.is_number_float() || right.is_number_float()) {
    const auto& floating = left.is_number_float() ? left : right;
    const auto& integer = left.is_number_float() ? right : left;
    const auto value = floating.get<double>();
    if (!std::isfinite(value) || std::trunc(value) != value) return false;
    if (integer.is_number_unsigned())
      return value >= 0 && value < 0x1p64 &&
             static_cast<std::uint64_t>(value) == integer.get<std::uint64_t>();
    return value >= -0x1p63 && value < 0x1p63 &&
           static_cast<std::int64_t>(value) == integer.get<std::int64_t>();
  }
  if (left.is_number_unsigned() && right.is_number_unsigned())
    return left.get<std::uint64_t>() == right.get<std::uint64_t>();
  if (!left.is_number_unsigned() && !right.is_number_unsigned())
    return left.get<std::int64_t>() == right.get<std::int64_t>();
  const auto& unsigned_value = left.is_number_unsigned() ? left : right;
  const auto& signed_value = left.is_number_unsigned() ? right : left;
  const auto value = signed_value.get<std::int64_t>();
  return value >= 0 && static_cast<std::uint64_t>(value) ==
                           unsigned_value.get<std::uint64_t>();
}

// JSONB may normalize an unknown 1e0 to integer 1. Never compare a large
// integer by first rounding it to double: the half-open range checks above
// permit only safe, exact float-to-integer casts. Both inputs are preflighted.
auto same_json(const Json& left, const Json& right, std::size_t depth = 0)
    -> bool {
  if (depth > MAX_DEPTH + 3) return false;
  if (left.is_number() && right.is_number()) return same_number(left, right);
  if (left.type() != right.type()) return false;
  if (left.is_array()) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
      if (!same_json(left[index], right[index], depth + 1)) return false;
    return true;
  }
  if (left.is_object()) {
    if (left.size() != right.size()) return false;
    for (auto iterator = left.begin(); iterator != left.end(); ++iterator) {
      const auto found = right.find(iterator.key());
      if (found == right.end() ||
          !same_json(iterator.value(), *found, depth + 1))
        return false;
    }
    return true;
  }
  return left == right;
}

auto manifest_bytes(const CapturedArtifact& artifact)
    -> std::expected<std::array<std::string_view, 3>, BindingError> {
  std::array<std::string_view, 3> bytes{};
  constexpr std::array<std::string_view, 3> paths{
      "manifest.json", "capabilities.json", "panels.json"};
  std::size_t size = 0;
  std::size_t nodes = 0;
  for (std::size_t index = 0; index < paths.size(); ++index) {
    const auto* file = artifact.find(paths[index]);
    if (!file && index == 2) continue; // Historical packages may omit panels.
    if (!file || file->kind != restore::ArtifactKind::REGULAR ||
        file->content.empty())
      return std::unexpected(BindingError::INVALID_CAPTURE);
    if (file->content.size() > MAX_METADATA - size)
      return std::unexpected(BindingError::LIMIT_EXCEEDED);
    size += file->content.size();
    ManifestPreflight validator{nodes};
    if (!Json::sax_parse(file->content, &validator))
      return std::unexpected(validator.exceeded()
                                 ? BindingError::LIMIT_EXCEEDED
                                 : BindingError::INVALID_CAPTURE);
    bytes[index] = file->content;
  }
  return bytes;
}

// One statement snapshot; LIMIT bounds row count before text leaves PG, and
// the window cap substitutes NULL BEFORE any oversized payload is materialized
// by libpq. Includes foreign/unsupported rows colliding with this namespace.
constexpr auto SQL =
    "WITH package AS (SELECT * FROM plinth.packages WHERE id=$1::uuid), "
    "raw AS (SELECT 'package'::text AS tag,jsonb_build_object("
    "'name',name,'version',version,'provenance',provenance,'manifest',manifest_"
    "json,"
    "'entry_point',entry_point,'frontend_mount',frontend_mount,'frontend_entry'"
    ",frontend_entry,"
    "'manifest_checksum',manifest_checksum)::text AS payload FROM package "
    "UNION ALL SELECT "
    "'panel',jsonb_build_object('id',pn.panel_id,'type',pn.panel_type,"
    "'format',pn.validated_format_version,'slot',pn.slot_type,'declaration',pn."
    "declaration,"
    "'logical_install_order',pn.logical_install_order::text,'rule_owned',"
    "EXISTS("
    "SELECT 1 FROM plinth.rbac_rules r WHERE "
    "r.rule=pn.declaration->>'rbac_rule' "
    "AND r.extension_name=$2 AND r.orphaned_at IS NULL))::text "
    "FROM plinth.panels pn JOIN package p ON p.id=pn.package_id "
    "UNION ALL SELECT 'capability',jsonb_build_object('namespace',c.namespace,"
    "'version',c.version,'function',c.function,'signature',c.signature,'scope',"
    "c.scope,"
    "'description',c.description,'rbac_rule',c.rbac_rule,'enabled',c.enabled,"
    "'provider_type',c.provider_type,'extension_name',c.extension_name,'rule_"
    "owned',EXISTS("
    "SELECT 1 FROM plinth.rbac_rules r WHERE r.rule=c.rbac_rule "
    "AND r.extension_name=$2 AND r.orphaned_at IS NULL))::text "
    "FROM plinth.capabilities c WHERE EXISTS(SELECT 1 FROM package) AND "
    "(c.extension_name=$2 OR c.namespace=$2 OR "
    "left(c.signature,length($2)+1)=$2||':')), "
    "bounded AS MATERIALIZED (SELECT tag,payload FROM raw LIMIT 4098) "
    "SELECT tag,CASE WHEN sum(octet_length(payload)) OVER()<=1048576 THEN "
    "payload "
    "ELSE NULL END FROM bounded";

struct Rows {
  Json package;
  std::map<std::string, Json> panels;
  std::vector<std::string> capabilities;
};

auto read_rows(name_operation::NameOperation& operation,
               const GenerationKey& key) -> std::expected<Rows, BindingError> {
  const std::array<const char*, 2> values{key.package_id.c_str(),
                                          key.name.c_str()};
  const Result result{db::exec_params(operation.connection(), SQL, 2, nullptr,
                                      values.data(), nullptr, nullptr, 0),
                      PQclear};
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQnfields(result.get()) != 2)
    return std::unexpected(BindingError::UNAVAILABLE);
  if (PQntuples(result.get()) > static_cast<int>(MAX_ROWS + 1))
    return std::unexpected(BindingError::LIMIT_EXCEEDED);
  Rows rows;
  std::size_t nodes = 0;
  for (int index = 0; index < PQntuples(result.get()); ++index) {
    if (PQgetisnull(result.get(), index, 1) ||
        PQgetlength(result.get(), index, 1) > static_cast<int>(MAX_METADATA))
      return std::unexpected(BindingError::LIMIT_EXCEEDED);
    const std::string_view raw{
        PQgetvalue(result.get(), index, 1),
        static_cast<std::size_t>(PQgetlength(result.get(), index, 1))};
    // Stored package metadata wraps a captured manifest in two object levels.
    ManifestPreflight validator{nodes, MAX_DEPTH + 2};
    if (!Json::sax_parse(raw, &validator))
      return std::unexpected(validator.exceeded()
                                 ? BindingError::LIMIT_EXCEEDED
                                 : BindingError::DECLARATION_MISMATCH);
    auto payload = Json::parse(raw, nullptr, false);
    if (!payload.is_object())
      return std::unexpected(BindingError::DECLARATION_MISMATCH);
    const std::string_view tag{PQgetvalue(result.get(), index, 0)};
    if (tag == "package") {
      if (!rows.package.is_null())
        return std::unexpected(BindingError::GENERATION_MISMATCH);
      rows.package = std::move(payload);
    } else if (tag == "panel") {
      if (!payload.contains("id") || !payload["id"].is_string() ||
          !rows.panels.emplace(payload["id"].get<std::string>(), payload)
               .second)
        return std::unexpected(BindingError::DECLARATION_MISMATCH);
    } else if (tag == "capability") {
      rows.capabilities.push_back(payload.dump());
    } else
      return std::unexpected(BindingError::UNAVAILABLE);
  }
  if (!rows.package.is_object() ||
      rows.package.value("name", std::string{}) != key.name ||
      rows.package.value("version", std::string{}) != key.version)
    return std::unexpected(BindingError::GENERATION_MISMATCH);
  std::ranges::sort(rows.capabilities);
  return rows;
}

struct Manifests {
  PackageManifest package;
  CapabilityManifest capabilities;
  PanelsManifest panels;
  bool panels_present = false;
};

auto parse_manifests(const std::array<std::string_view, 3>& bytes,
                     const GenerationKey& key, const Json& stored)
    -> std::expected<Manifests, BindingError> {
  if (!stored.contains("provenance") || !stored["provenance"].is_string())
    return std::unexpected(BindingError::GENERATION_MISMATCH);
  const auto provenance = stored["provenance"].get<std::string>();
  if (provenance != "user" && provenance != "bundled")
    return std::unexpected(BindingError::GENERATION_MISMATCH);
  auto package = PackageManifest::parse(bytes[0], "captured manifest.json",
                                        provenance == "bundled");
  auto capabilities =
      CapabilityManifest::parse(bytes[1], "captured capabilities.json");
  if (!package.value || !capabilities.value ||
      package.value->name != key.name || package.value->version != key.version)
    return std::unexpected(BindingError::INVALID_CAPTURE);
  Manifests manifests{.package = std::move(*package.value),
                      .capabilities = std::move(*capabilities.value),
                      .panels = {},
                      .panels_present = !bytes[2].empty()};
  if (manifests.panels_present) {
    auto panels = PanelsManifest::parse(bytes[2], "captured panels.json");
    if (!panels.value) return std::unexpected(BindingError::INVALID_CAPTURE);
    manifests.panels = std::move(*panels.value);
  }
  if (manifests.panels.panels.size() + manifests.capabilities.provides.size() >
      MAX_ROWS)
    return std::unexpected(BindingError::LIMIT_EXCEEDED);
  Json expected{{"name", key.name},
                {"version", key.version},
                {"provenance", provenance},
                {"manifest", Json::parse(bytes[0])},
                {"entry_point", manifests.package.entry_point},
                {"frontend_mount", nullptr},
                {"frontend_entry", nullptr},
                {"manifest_checksum", packages::detail::sha256_hex(bytes[0])}};
  if (manifests.package.frontend) {
    expected["frontend_mount"] = manifests.package.frontend->mount;
    expected["frontend_entry"] = manifests.package.frontend->entry;
  }
  if (!same_json(expected, stored))
    return std::unexpected(BindingError::DECLARATION_MISMATCH);
  return manifests;
}

auto bind_capabilities(const Manifests& manifests, const GenerationKey& key,
                       const Rows& rows) -> std::expected<void, BindingError> {
  std::vector<std::string> expected;
  std::set<std::pair<std::string, std::string>> signatures;
  std::set<std::string> declared_signatures;
  for (const auto& provided : manifests.capabilities.provides) {
    const auto signature = provided.namespace_ + ":" +
                           std::to_string(provided.version) + ":" +
                           provided.function;
    if (provided.namespace_ != key.name ||
        (provided.scope != "instance" && provided.scope != "user") ||
        !provided.rbac_rule ||
        !provided.rbac_rule->starts_with(key.name + ".") ||
        !signatures.emplace(signature, provided.scope).second)
      return std::unexpected(BindingError::DECLARATION_MISMATCH);
    declared_signatures.insert(signature);
    expected.push_back(Json{
        {"namespace", provided.namespace_},
        {"version", provided.version},
        {"function", provided.function},
        {"signature", signature},
        {"scope", provided.scope},
        {"description", provided.description},
        {"rbac_rule", *provided.rbac_rule},
        {"enabled", true},
        {"provider_type", "extension"},
        {"extension_name", key.name},
        {"rule_owned",
         true}}.dump());
  }
  std::ranges::sort(expected);
  if (expected != rows.capabilities)
    return std::unexpected(BindingError::DECLARATION_MISMATCH);
  for (const auto& panel : manifests.panels.panels)
    if (panel.typed_declaration && panel.type == PanelType::FLOAT &&
        (!panel.capability || !declared_signatures.contains(*panel.capability)))
      return std::unexpected(BindingError::DECLARATION_MISMATCH);
  return {};
}

auto panel_asset_exists(const CapturedArtifact& artifact, std::string_view path)
    -> bool {
  if (path.empty() || path.size() > restore::INVENTORY_PATH_MAX_BYTES ||
      path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos)
    return false;
  for (std::size_t begin = 0; begin <= path.size();) {
    const auto end = path.find('/', begin);
    const auto part =
        path.substr(begin, end == std::string_view::npos ? end : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  const auto* file = artifact.find("client/panels/" + std::string{path});
  return file && file->kind == restore::ArtifactKind::REGULAR;
}

auto bind_panels(const Manifests& manifests, const CapturedArtifact& artifact,
                 const Rows& rows) -> std::expected<void, BindingError> {
  if (manifests.panels.panels.size() != rows.panels.size())
    return std::unexpected(BindingError::DECLARATION_MISMATCH);
  std::set<std::string> seen;
  for (const auto& panel : manifests.panels.panels) {
    const auto found = rows.panels.find(panel.id);
    if (!seen.insert(panel.id).second || found == rows.panels.end() ||
        !panel_asset_exists(artifact, panel.client_path))
      return std::unexpected(BindingError::DECLARATION_MISMATCH);
    const bool typed = manifests.panels.format_version == 2;
    const bool ordered = typed && panel.type == PanelType::FLOAT;
    Json stored_document{
        {"panels", Json::array({found->second.at("declaration")})}};
    if (typed) stored_document["format_version"] = 2;
    const auto parsed =
        PanelsManifest::parse(stored_document.dump(), "registered panel");
    if (!parsed.value || parsed.value->panels.size() != 1 ||
        parsed.value->panels[0].typed_declaration != typed ||
        parsed.value->panels[0].id != panel.id)
      return std::unexpected(BindingError::DECLARATION_MISMATCH);
    const auto ordinal = found->second.at("logical_install_order");
    if ((ordered && !valid_ordinal(ordinal)) ||
        (!ordered && !ordinal.is_null()))
      return std::unexpected(BindingError::DECLARATION_MISMATCH);
    const Json expected{{"id", panel.id},
                        {"type", typed ? panel_type_to_string(panel.type)
                                       : std::string_view{"primary"}},
                        {"format", typed ? 2 : 0},
                        {"slot", nullptr},
                        {"declaration", panel.declaration()},
                        {"logical_install_order", ordinal},
                        {"rule_owned", true}};
    if (!same_json(expected, found->second))
      return std::unexpected(BindingError::DECLARATION_MISMATCH);
  }
  return {};
}

auto transcript(const Manifests& manifests, const Rows& rows,
                const GenerationKey& key)
    -> std::expected<std::string, BindingError> {
  auto package_text = manifests.package.serialize();
  if (package_text.size() > MAX_METADATA)
    return std::unexpected(BindingError::LIMIT_EXCEEDED);
  auto capabilities_text = manifests.capabilities.serialize();
  if (capabilities_text.size() > MAX_METADATA - package_text.size())
    return std::unexpected(BindingError::LIMIT_EXCEEDED);
  auto panels_text =
      manifests.panels_present ? manifests.panels.serialize() : std::string{};
  if (panels_text.size() >
      MAX_METADATA - package_text.size() - capabilities_text.size())
    return std::unexpected(BindingError::LIMIT_EXCEEDED);
  Json panels = Json::array();
  for (const auto& [id, panel] : rows.panels) {
    static_cast<void>(id);
    panels.push_back(panel.dump());
  }
  const Json document{{"profile", 1},
                      {"package_id", key.package_id},
                      {"name", key.name},
                      {"version", key.version},
                      {"provenance", rows.package.at("provenance")},
                      {"manifest", std::move(package_text)},
                      {"capabilities", std::move(capabilities_text)},
                      {"panels", manifests.panels_present
                                     ? Json(std::move(panels_text))
                                     : Json(nullptr)},
                      {"registered_panels", panels},
                      {"registered_capabilities", rows.capabilities}};
  const auto encoded = restore::detail::canonical_json(document, MAX_METADATA);
  if (!encoded) return std::unexpected(BindingError::LIMIT_EXCEEDED);
  std::string input;
  input.reserve(DOMAIN.size() + 1 + encoded->size());
  input.append(DOMAIN);
  input.push_back('\0');
  input.append(*encoded);
  return packages::detail::sha256_hex(input);
}
} // namespace

auto bind_registered_generation(name_operation::NameOperation& operation,
                                const CapturedArtifact& artifact,
                                Budget& budget)
    -> std::expected<std::string, BindingError> try {
  // Explicit copy envelope, not a total-process/DOM/libpq allocator bound.
  // Ticket remains held until all owned raw/normalized/transcript bytes die.
  const auto working = budget.try_reserve(WORKING_BYTES);
  if (!working) return std::unexpected(BindingError::LIMIT_EXCEEDED);
  const auto& key = artifact.key();
  if (key.profile != 1 || !valid_uuid(key.package_id) ||
      key.name != operation.name() ||
      artifact.records().size() > restore::INVENTORY_MAX_RECORDS)
    return std::unexpected(BindingError::INVALID_CAPTURE);
  if (auto owned = operation.assert_owned(); !owned)
    return std::unexpected(owned.error() == name_operation::NameError::NOT_OWNED
                               ? BindingError::LOCK_NOT_OWNED
                               : BindingError::UNAVAILABLE);
  auto bytes = manifest_bytes(artifact);
  if (!bytes) return std::unexpected(bytes.error());
  const auto rows = read_rows(operation, key);
  if (!rows) return std::unexpected(rows.error());
  const auto manifests = parse_manifests(*bytes, key, rows->package);
  if (!manifests) return std::unexpected(manifests.error());
  if (auto capabilities = bind_capabilities(*manifests, key, *rows);
      !capabilities)
    return std::unexpected(capabilities.error());
  if (auto panels = bind_panels(*manifests, artifact, *rows); !panels)
    return std::unexpected(panels.error());
  return transcript(*manifests, *rows, key);
} catch (const std::bad_alloc&) {
  return std::unexpected(BindingError::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(BindingError::LIMIT_EXCEEDED);
} catch (const Json::exception&) {
  return std::unexpected(BindingError::DECLARATION_MISMATCH);
} catch (const std::exception&) {
  return std::unexpected(BindingError::UNAVAILABLE);
}
} // namespace plinth::packages::generation
