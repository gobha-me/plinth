#include "kernel/frontend/persistence_projection.hpp"

#include "kernel/packages/detail/launcher_validation.hpp"
#include "kernel/packages/restore_digest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <new>
#include <stdexcept>

namespace plinth::frontend {

auto projection_detail::checked_codec(
    std::expected<std::string, packages::restore::CodecError> result)
    -> std::optional<std::string> {
  if (result) return std::move(*result);
  switch (result.error()) {
    case packages::restore::CodecError::LIMIT_EXCEEDED:
    case packages::restore::CodecError::HASHING_FAILED:
      throw ProjectionUnavailable{};
    case packages::restore::CodecError::TOO_LARGE:
    case packages::restore::CodecError::INVALID_JSON:
    case packages::restore::CodecError::DUPLICATE_KEY:
    case packages::restore::CodecError::INVALID_POLICY:
    case packages::restore::CodecError::CONFLICTING_BINDING:
    case packages::restore::CodecError::INVALID_INVENTORY:
    case packages::restore::CodecError::INVALID_SCHEMA: return std::nullopt;
  }
  throw ProjectionUnavailable{};
}

namespace {

auto context_string(std::string_view value, std::size_t cap) -> bool {
  return value.size() <= cap && value.find('\0') == std::string_view::npos &&
         packages::detail::unicode_scalar_count(value).has_value();
}

auto selected(const PersistenceProjection& projection, std::string_view key)
    -> bool {
  return std::ranges::find(projection.approved_keys, key) !=
         projection.approved_keys.end();
}

auto match_projection(const packages::restore::ProtectedPolicy& snapshot,
                      std::string_view application, std::string_view panel,
                      std::string_view capability,
                      std::string_view artifact_digest,
                      const packages::PanelContextSchema& schema)
    -> std::optional<PersistenceProjection> {
  const auto digest = projection_detail::checked_codec(
      packages::restore::schema_digest(schema));
  if (!digest) return std::nullopt;
  const auto& policy = snapshot.parsed();
  for (const auto& entry : policy.entries) {
    if (entry.application_id != application || entry.panel_id != panel ||
        entry.capability != capability ||
        entry.artifact_digest != artifact_digest ||
        entry.schema_digest != *digest)
      continue;
    PersistenceProjection result{.projection_contract =
                                     entry.projection_contract,
                                 .artifact_digest = entry.artifact_digest,
                                 .schema_digest = entry.schema_digest,
                                 .policy_revision = policy.policy_revision,
                                 .approved_keys = {}};
    for (const auto& approved : entry.fields) {
      const auto field = schema.fields.find(approved.key);
      if (field == schema.fields.end() ||
          field->second.meaning != approved.meaning)
        return std::nullopt;
      if (std::ranges::find(schema.persist_keys, approved.key) !=
          schema.persist_keys.end())
        result.approved_keys.push_back(approved.key);
    }
    if (std::ranges::any_of(schema.fields, [&](const auto& field) {
          return field.second.required && !selected(result, field.first);
        }))
      return std::nullopt;
    std::ranges::sort(result.approved_keys);
    return result;
  }
  return std::nullopt;
}

auto validate_context(const packages::PanelContextSchema& schema,
                      const nlohmann::json& context) -> bool {
  if (!context.is_object() || context.size() > 16 ||
      !projection_detail::checked_codec(
          packages::restore::canonical_schema(schema)))
    return false;
  // Bound the unencoded data before serializing; every value is a flat string.
  for (const auto& [key, value] : context.items()) {
    const auto field = schema.fields.find(key);
    if (key.empty() || key == "__proto__" || key == "prototype" ||
        key == "constructor" || !context_string(key, 64) ||
        field == schema.fields.end() || !value.is_string() ||
        !context_string(value.get_ref<const std::string&>(),
                        field->second.max_bytes))
      return false;
  }
  if (std::ranges::any_of(schema.fields, [&](const auto& field) {
        return field.second.required && !context.contains(field.first);
      }))
    return false;
  return context.dump().size() <= 4096;
}

auto project_context(const PersistenceProjection& projection,
                     const packages::PanelContextSchema& schema,
                     const nlohmann::json& context)
    -> std::optional<nlohmann::json> {
  try {
    const auto digest = projection_detail::checked_codec(
        packages::restore::schema_digest(schema));
    if (!digest || *digest != projection.schema_digest ||
        !validate_context(schema, context))
      return std::nullopt;
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [key, value] : context.items()) {
      if (selected(projection, key) &&
          std::ranges::find(schema.persist_keys, key) !=
              schema.persist_keys.end())
        result[key] = value;
    }
    if (!validate_context(schema, result)) return std::nullopt;
    return result;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

} // namespace

auto match_persistence_projection(
    const packages::restore::ProtectedPolicy& snapshot,
    std::string_view application, std::string_view panel,
    std::string_view capability, std::string_view artifact_digest,
    const packages::PanelContextSchema& schema)
    -> std::optional<PersistenceProjection> {
  return projection_detail::operational_boundary([&] {
    return match_projection(snapshot, application, panel, capability,
                            artifact_digest, schema);
  });
}

auto validate_float_context(const packages::PanelContextSchema& schema,
                            const nlohmann::json& context) -> bool {
  return projection_detail::operational_boundary(
      [&] { return validate_context(schema, context); });
}

auto project_float_context(const PersistenceProjection& projection,
                           const packages::PanelContextSchema& schema,
                           const nlohmann::json& context)
    -> std::optional<nlohmann::json> {
  return projection_detail::operational_boundary(
      [&] { return project_context(projection, schema, context); });
}

} // namespace plinth::frontend
