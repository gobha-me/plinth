#include "kernel/frontend/surface_authority.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <variant>

namespace plinth::frontend::authority {
namespace {

using Json = nlohmann::json;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
constexpr std::size_t RULES_MAX = 4096;
constexpr std::size_t ROW_MAX = std::size_t{64} * 1024;

auto uuid(std::string_view value) -> bool {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto byte = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (byte != '-') return false;
    } else if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return false;
  }
  return true;
}
auto identifier(std::string_view value, bool panel) -> bool {
  if (value.size() < (panel ? 1U : 2U) || value.size() > 64 ||
      value.front() < 'a' || value.front() > 'z')
    return false;
  return std::ranges::all_of(value, [panel](char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '-' || (panel && byte == '_');
  });
}
auto ordinal(std::string_view value) -> bool {
  if (value.empty() || value.front() == '0' || value.size() > 20) return false;
  std::uint64_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  return result.ec == std::errc{} &&
         result.ptr == value.data() + value.size() && parsed > 0;
}
auto valid_auth(const auth::AuthContext& auth) -> bool {
  if (!uuid(auth.user_id) || auth.token_hash.size() != 64 ||
      !std::ranges::all_of(auth.token_hash,
                           [](char byte) {
                             return (byte >= '0' && byte <= '9') ||
                                    (byte >= 'a' && byte <= 'f');
                           }) ||
      (auth.credential_source != plinth::auth::CredentialSource::COOKIE &&
       auth.credential_source != plinth::auth::CredentialSource::BEARER))
    return false;
  return auth.auth_type == "session"
             ? uuid(auth.session_id) && auth.pat_id.empty()
             : auth.auth_type == "pat" && uuid(auth.pat_id) &&
                   auth.session_id.empty();
}
auto kind_name(SurfaceKind kind) -> std::string_view {
  switch (kind) {
    case SurfaceKind::PRIMARY: return "primary";
    case SurfaceKind::FLOAT: return "float";
    case SurfaceKind::TRAY: return "tray";
  }
  return {};
}

auto auth_cte(const auth::AuthContext& auth) -> std::string {
  const std::string table =
      auth.auth_type == "session" ? "plinth.sessions" : "plinth.pats";
  const std::string expiry =
      auth.auth_type == "session"
          ? "c.expires_at > clock_timestamp()"
          : "(c.expires_at IS NULL OR c.expires_at > clock_timestamp())";
  return "WITH credential AS (SELECT u.id,u.username FROM plinth.users u "
         "JOIN " +
         table +
         " c ON c.user_id=u.id WHERE u.id=$1::uuid AND c.id=$3::uuid AND "
         "c.token_hash=$2 "
         "AND c.revoked_at IS NULL AND u.disabled_at IS NULL AND " +
         expiry +
         "), "
         "effective_rules AS (SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
         "JOIN plinth.group_rules gr ON gr.rule_id=r.id JOIN plinth.groups g "
         "ON g.id=gr.group_id "
         "WHERE r.orphaned_at IS NULL AND EXISTS (SELECT 1 FROM credential u "
         "WHERE g.name='everyone' OR EXISTS (SELECT 1 FROM "
         "plinth.group_members gm "
         "WHERE gm.group_id=g.id AND gm.user_id=u.id))), "
         "auth_reply AS (SELECT 'auth'::text AS "
         "tag,jsonb_build_object('username', "
         "CASE WHEN octet_length(u.username)<=256 THEN u.username ELSE NULL "
         "END, 'rules', "
         "(SELECT COALESCE(jsonb_agg(rule ORDER BY rule),'[]'::jsonb) FROM "
         "(SELECT CASE WHEN octet_length(rule)<=256 THEN rule ELSE NULL END AS "
         "rule "
         "FROM effective_rules ORDER BY rule LIMIT 4097) bounded))::text AS "
         "payload FROM credential u) ";
}

constexpr std::string_view AUTH_REPLY = "SELECT tag,payload FROM auth_reply";
// The lateral lookup selects the most specific stored row FIRST. Unsupported
// user rows, disabled/foreign providers and foreign/orphaned rules cannot make
// the instance row an alternative candidate. These are not permission grants.
constexpr std::string_view ELIGIBLE =
    "SELECT p.id::text AS generation,p.name AS application_id,p.version, "
    "pn.panel_id, "
    "pn.panel_type,pn.logical_install_order, "
    "CASE WHEN octet_length(pn.declaration::text)<=32768 THEN pn.declaration "
    "ELSE NULL END AS declaration, "
    "c.rbac_rule AS capability_rule,c.provider_type,c.extension_name AS "
    "provider_name,c.scope AS capability_scope "
    "FROM plinth.packages p JOIN plinth.panels pn ON pn.package_id=p.id "
    "JOIN plinth.rbac_rules pr ON pr.rule=pn.declaration->>'rbac_rule' "
    "AND pr.extension_name=p.name AND pr.orphaned_at IS NULL "
    "LEFT JOIN LATERAL (SELECT c.* FROM plinth.capabilities c "
    "WHERE c.signature=pn.declaration->>'capability' "
    "ORDER BY (c.scope='user') DESC,c.id LIMIT 1) c ON pn.panel_type='float' "
    "LEFT JOIN plinth.rbac_rules cr ON cr.rule=c.rbac_rule AND "
    "cr.extension_name=p.name AND cr.orphaned_at IS NULL "
    "WHERE EXISTS (SELECT 1 FROM credential) AND p.application_ready "
    "AND p.state IN ('ACTIVE','ACTIVE_FLAGGED') AND "
    "pn.validated_format_version=2 "
    "AND EXISTS (SELECT 1 FROM effective_rules e WHERE e.rule=pr.rule OR "
    "e.rule='kernel.admin') "
    "AND (pn.panel_type!='primary' OR "
    "pn.declaration->'accepts_navigation_intent'='true'::jsonb) "
    "AND (pn.panel_type!='float' OR (c.enabled AND c.scope='instance' "
    "AND c.provider_type='extension' AND c.extension_name=p.name AND "
    "c.namespace=p.name "
    "AND c.signature=c.namespace||':'||c.version::text||':'||c.function AND "
    "cr.id IS NOT NULL "
    "AND NOT EXISTS(SELECT 1 FROM plinth.capabilities ambiguous WHERE "
    "ambiguous.signature=c.signature "
    "AND ambiguous.scope=c.scope AND ambiguous.id!=c.id) "
    "AND EXISTS (SELECT 1 FROM effective_rules e WHERE e.rule=c.rbac_rule OR "
    "e.rule='kernel.admin'))) ";

constexpr std::string_view TARGET_REPLY =
    "SELECT "
    "'target',jsonb_build_object('generation',generation,'application_id', "
    "CASE WHEN octet_length(application_id)<=64 THEN application_id ELSE NULL "
    "END, "
    "'version',CASE WHEN octet_length(version)<=256 THEN version ELSE NULL "
    "END, "
    "'panel_id',CASE WHEN octet_length(panel_id)<=64 THEN panel_id ELSE NULL "
    "END, "
    "'kind',panel_type,'declaration',declaration, "
    "'logical_install_order',logical_install_order::text, "
    "'capability_rule',CASE WHEN octet_length(capability_rule)<=256 THEN "
    "capability_rule ELSE NULL END, "
    "'provider_type',provider_type,'provider_name',CASE WHEN "
    "octet_length(provider_name)<=64 THEN provider_name ELSE NULL END, "
    "'capability_scope',capability_scope)::text FROM selected";

auto query(PGconn& connection, const std::string& sql,
           const std::vector<std::string>& values) -> Result {
  std::vector<const char*> pointers;
  pointers.reserve(values.size());
  for (const auto& value : values)
    pointers.push_back(value.c_str());
  return {db::exec_params(&connection, sql.c_str(),
                          static_cast<int>(pointers.size()), nullptr,
                          pointers.data(), nullptr, nullptr, 0),
          PQclear};
}
auto auth_values(const auth::AuthContext& auth) -> std::vector<std::string> {
  return {auth.user_id, auth.token_hash,
          auth.auth_type == "session" ? auth.session_id : auth.pat_id};
}
auto parse_payload(PGresult& result, int row, std::size_t maximum)
    -> std::optional<Json> {
  if (PQgetisnull(&result, row, 1) != 0 || PQgetlength(&result, row, 1) < 0 ||
      static_cast<std::size_t>(PQgetlength(&result, row, 1)) > maximum)
    return std::nullopt;
  auto value = Json::parse(PQgetvalue(&result, row, 1), nullptr, false);
  if (!value.is_object()) return std::nullopt;
  return value;
}
auto parse_caller(const Json& data, const auth::AuthContext& auth)
    -> std::expected<capabilities::UserContext, Error> {
  if (data.size() != 2 || !data.contains("username") ||
      !data["username"].is_string() || !data.contains("rules") ||
      !data["rules"].is_array() || data["rules"].size() > RULES_MAX) {
    return std::unexpected(Error::UNAVAILABLE);
  }
  capabilities::UserContext caller{.user_id = auth.user_id,
                                   .username =
                                       data["username"].get<std::string>(),
                                   .auth_type = auth.auth_type,
                                   .effective_rules = {},
                                   .session_id = auth.session_id,
                                   .ip_address = {}};
  if (caller.username.empty() || caller.username.size() > 256)
    return std::unexpected(Error::UNAVAILABLE);
  for (const auto& rule : data["rules"]) {
    if (!rule.is_string() || rule.get_ref<const std::string&>().size() > 256 ||
        !packages::detail::is_rbac_rule_name_valid(
            rule.get_ref<const std::string&>())) {
      return std::unexpected(Error::UNAVAILABLE);
    }
    caller.effective_rules.push_back(rule.get<std::string>());
  }
  return caller;
}
auto parse_target(const Json& data) -> std::optional<SurfaceTarget> {
  if (data.size() != 11) return std::nullopt;
  for (const auto key :
       {"generation", "application_id", "version", "panel_id", "kind"}) {
    if (!data.contains(key) || !data[key].is_string()) return std::nullopt;
  }
  const auto generation = data["generation"].get<std::string>();
  const auto application = data["application_id"].get<std::string>();
  const auto version = data["version"].get<std::string>();
  const auto panel = data["panel_id"].get<std::string>();
  const auto kind = data["kind"].get<std::string>();
  if (!uuid(generation) || !identifier(application, false) ||
      !identifier(panel, true) || version.empty() || version.size() > 256 ||
      !packages::detail::unicode_scalar_count(version))
    return std::nullopt;
  SurfaceKind parsed_kind;
  if (kind == "float")
    parsed_kind = SurfaceKind::FLOAT;
  else if (kind == "primary")
    parsed_kind = SurfaceKind::PRIMARY;
  else if (kind == "tray")
    parsed_kind = SurfaceKind::TRAY;
  else
    return std::nullopt;
  if (!data.contains("declaration") || !data["declaration"].is_object())
    return std::nullopt;
  auto parsed = packages::PanelsManifest::parse(
      Json{{"format_version", 2},
           {"panels", Json::array({data["declaration"]})}}
          .dump(),
      "stored surface");
  if (!parsed.value || parsed.value->panels.size() != 1) return std::nullopt;
  auto declaration = std::move(parsed.value->panels.front());
  if (declaration.id != panel ||
      declaration.type !=
          (parsed_kind == SurfaceKind::FLOAT  ? packages::PanelType::FLOAT
           : parsed_kind == SurfaceKind::TRAY ? packages::PanelType::TRAY
                                              : packages::PanelType::PRIMARY))
    return std::nullopt;
  SurfaceTarget target{.generation = generation,
                       .application_id = application,
                       .version = version,
                       .kind = parsed_kind,
                       .declaration = std::move(declaration),
                       .logical_install_order = std::nullopt,
                       .capability_rule = {},
                       .provider_type = {},
                       .provider_name = {},
                       .capability_scope = {}};
  if (parsed_kind == SurfaceKind::FLOAT) {
    if (!data["logical_install_order"].is_string() ||
        !ordinal(data["logical_install_order"].get_ref<const std::string&>()))
      return std::nullopt;
    target.logical_install_order =
        data["logical_install_order"].get<std::string>();
    for (auto [key, out] :
         {std::pair{"capability_rule", &target.capability_rule},
          {"provider_type", &target.provider_type},
          {"provider_name", &target.provider_name},
          {"capability_scope", &target.capability_scope}}) {
      if (!data[key].is_string() ||
          data[key].get_ref<const std::string&>().size() > 256)
        return std::nullopt;
      *out = data[key].get<std::string>();
    }
    if (!packages::detail::is_rbac_rule_name_valid(target.capability_rule) ||
        target.provider_type != "extension" ||
        target.provider_name != application ||
        target.capability_scope != "instance")
      return std::nullopt;
  }
  return target;
}

struct Rows {
  capabilities::UserContext caller;
  std::vector<SurfaceTarget> targets;
};
auto read_rows(PGresult* result, const auth::AuthContext& auth,
               std::size_t maximum) -> std::expected<Rows, Error> {
  if (!result || PQresultStatus(result) != PGRES_TUPLES_OK ||
      PQnfields(result) != 2)
    return std::unexpected(Error::UNAVAILABLE);
  if (PQntuples(result) == 0) return std::unexpected(Error::UNAUTHENTICATED);
  if (PQntuples(result) > static_cast<int>(maximum + 1))
    return std::unexpected(Error::UNAVAILABLE);
  Rows rows;
  bool found_auth = false;
  for (int index = 0; index < PQntuples(result); ++index) {
    const std::string_view tag{PQgetvalue(result, index, 0)};
    auto payload = parse_payload(
        *result, index, tag == "auth" ? std::size_t{2} * 1024 * 1024 : ROW_MAX);
    if (!payload) return std::unexpected(Error::UNAVAILABLE);
    if (tag == "auth") {
      if (found_auth) return std::unexpected(Error::UNAVAILABLE);
      auto caller = parse_caller(*payload, auth);
      if (!caller) return std::unexpected(caller.error());
      rows.caller = std::move(*caller);
      found_auth = true;
    } else if (tag == "target") {
      auto target = parse_target(*payload);
      if (!target) return std::unexpected(Error::UNAVAILABLE);
      rows.targets.push_back(std::move(*target));
    } else
      return std::unexpected(Error::UNAVAILABLE);
  }
  if (!found_auth) return std::unexpected(Error::UNAUTHENTICATED);
  return rows;
}

} // namespace

auto authenticate(PGconn& connection, const auth::AuthContext& auth)
    -> std::expected<capabilities::UserContext, Error> try {
  if (!valid_auth(auth)) return std::unexpected(Error::UNAUTHENTICATED);
  auto result = query(connection, auth_cte(auth) + std::string{AUTH_REPLY},
                      auth_values(auth));
  auto rows = read_rows(result.get(), auth, 0);
  if (!rows) return std::unexpected(rows.error());
  return std::move(rows->caller);
} catch (const std::exception&) {
  return std::unexpected(Error::UNAVAILABLE);
}

auto require_rule(PGconn& connection, const auth::AuthContext& auth,
                  std::string_view rule)
    -> std::expected<capabilities::UserContext, Error> {
  if (rule.size() > 256 || !packages::detail::is_rbac_rule_name_valid(rule))
    return std::unexpected(Error::INVALID);
  auto caller = authenticate(connection, auth);
  if (!caller) return std::unexpected(caller.error());
  if (std::ranges::none_of(
          caller->effective_rules, [rule](const auto& granted) {
            return granted == rule || granted == "kernel.admin";
          }))
    return std::unexpected(Error::DENIED);
  return caller;
}

auto reviewer_is_admin(PGconn& connection, std::string_view user_id)
    -> std::expected<bool, Error> try {
  if (!uuid(user_id)) return std::unexpected(Error::INVALID);
  const std::vector<std::string> values{std::string{user_id}};
  auto result = query(
      connection,
      "SELECT EXISTS(SELECT 1 FROM plinth.users u JOIN plinth.groups g ON true "
      "JOIN plinth.group_rules gr ON gr.group_id=g.id JOIN plinth.rbac_rules r "
      "ON r.id=gr.rule_id "
      "WHERE u.id=$1::uuid AND u.disabled_at IS NULL AND r.rule='kernel.admin' "
      "AND r.orphaned_at IS NULL "
      "AND (g.name='everyone' OR EXISTS(SELECT 1 FROM plinth.group_members gm "
      "WHERE gm.group_id=g.id AND gm.user_id=u.id)))",
      values);
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) != 1 || PQgetisnull(result.get(), 0, 0))
    return std::unexpected(Error::UNAVAILABLE);
  return std::string_view{PQgetvalue(result.get(), 0, 0)} == "t";
} catch (const std::exception&) {
  return std::unexpected(Error::UNAVAILABLE);
}

auto require_shell_preference_rule(PGconn& connection,
                                   const auth::AuthContext& auth, bool write)
    -> std::expected<capabilities::UserContext, Error> try {
  if (!valid_auth(auth)) return std::unexpected(Error::UNAUTHENTICATED);
  auto values = auth_values(auth);
  values.emplace_back(write ? "shell:1:preferences.set"
                            : "shell:1:preferences.get");
  values.emplace_back(write ? "shell.preferences.write"
                            : "shell.preferences.read");
  const auto sql =
      auth_cte(auth) +
      "SELECT tag,(payload::jsonb || jsonb_build_object('registered',EXISTS("
      "SELECT 1 FROM plinth.capabilities c JOIN plinth.rbac_rules r ON "
      "r.rule=c.rbac_rule "
      "WHERE c.signature=$4 AND c.scope='instance' AND c.enabled "
      "AND c.namespace='shell' AND c.version=1 "
      "AND c.function=CASE WHEN $5='shell.preferences.write' THEN "
      "'preferences.set' ELSE 'preferences.get' END "
      "AND c.provider_type='extension' AND c.extension_name='shell' AND "
      "r.rule=$5 "
      "AND r.extension_name='shell' AND r.orphaned_at IS NULL "
      "AND NOT EXISTS(SELECT 1 FROM plinth.capabilities ambiguous WHERE "
      "ambiguous.signature=c.signature "
      "AND ambiguous.scope=c.scope AND ambiguous.id!=c.id) "
      "AND NOT EXISTS(SELECT 1 FROM plinth.capabilities unsupported WHERE "
      "unsupported.signature=$4 AND unsupported.scope='user'))))::text FROM "
      "auth_reply";
  auto result = query(connection, sql, values);
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
    return std::unexpected(Error::UNAVAILABLE);
  if (PQntuples(result.get()) == 0)
    return std::unexpected(Error::UNAUTHENTICATED);
  if (PQntuples(result.get()) != 1) return std::unexpected(Error::UNAVAILABLE);
  auto data = parse_payload(*result, 0, std::size_t{2} * 1024 * 1024);
  if (!data || !data->contains("registered") ||
      !(*data)["registered"].is_boolean())
    return std::unexpected(Error::UNAVAILABLE);
  const bool registered = (*data)["registered"].get<bool>();
  data->erase("registered");
  auto caller = parse_caller(*data, auth);
  if (!caller) return std::unexpected(caller.error());
  if (!registered ||
      std::ranges::none_of(caller->effective_rules, [write](const auto& rule) {
        return rule == "kernel.admin" ||
               rule == (write ? "shell.preferences.write"
                              : "shell.preferences.read");
      }))
    return std::unexpected(Error::DENIED);
  return caller;
} catch (const std::exception&) {
  return std::unexpected(Error::UNAVAILABLE);
}

auto lookup_surface(PGconn& connection, const auth::AuthContext& auth,
                    std::string_view application_id, std::string_view panel_id,
                    SurfaceKind kind)
    -> std::expected<LookupResult, Error> try {
  if (!valid_auth(auth)) return std::unexpected(Error::UNAUTHENTICATED);
  if (!identifier(application_id, false) || !identifier(panel_id, true) ||
      kind_name(kind).empty())
    return std::unexpected(Error::INVALID);
  auto values = auth_values(auth);
  values.emplace_back(application_id);
  values.emplace_back(panel_id);
  values.emplace_back(kind_name(kind));
  const auto sql =
      auth_cte(auth) + ", selected AS (" + std::string{ELIGIBLE} +
      "AND p.name=$4 AND pn.panel_id=$5 AND pn.panel_type=$6 LIMIT 2) " +
      std::string{AUTH_REPLY} + " UNION ALL " + std::string{TARGET_REPLY};
  auto result = query(connection, sql, values);
  auto rows = read_rows(result.get(), auth, 2);
  if (!rows) return std::unexpected(rows.error());
  if (rows->targets.size() > 1) return std::unexpected(Error::UNAVAILABLE);
  LookupResult found{.caller = std::move(rows->caller), .target = std::nullopt};
  if (!rows->targets.empty()) found.target = std::move(rows->targets.front());
  return found;
} catch (const std::exception&) {
  return std::unexpected(Error::UNAVAILABLE);
}

namespace {
auto catalog_float_prefix_impl(PGconn& connection,
                               const auth::AuthContext& auth,
                               std::optional<std::string_view> content_type,
                               std::size_t limit,
                               const std::optional<Position>& after)
    -> std::expected<SurfacePage, Error> try {
  if (!valid_auth(auth)) return std::unexpected(Error::UNAUTHENTICATED);
  if (limit == 0 || limit > 64 ||
      (content_type && !is_canonical_content_type(*content_type)) ||
      (after && (!after->logical_install_order ||
                 !ordinal(*after->logical_install_order) ||
                 !identifier(after->application_id, false) ||
                 !identifier(after->panel_id, true))))
    return std::unexpected(Error::INVALID);
  auto values = auth_values(auth);
  values.emplace_back(content_type.value_or(std::string_view{}));
  values.push_back(std::to_string(limit + 1));
  std::string cursor;
  if (after) {
    values.push_back(*after->logical_install_order);
    values.push_back(after->application_id);
    values.push_back(after->panel_id);
    cursor =
        "AND (pn.logical_install_order IS NULL OR "
        "(pn.logical_install_order,p.name COLLATE \"C\",pn.panel_id COLLATE "
        "\"C\") > ($6::numeric,$7 COLLATE \"C\",$8 COLLATE \"C\")) ";
  }
  const auto sql =
      auth_cte(auth) + ", selected AS (" + std::string{ELIGIBLE} +
      "AND pn.panel_type='float' AND ($4::text='' OR "
      "pn.declaration->'content_types' ? $4) " +
      cursor +
      "ORDER BY pn.logical_install_order ASC NULLS FIRST,p.name COLLATE "
      "\"C\",pn.panel_id COLLATE \"C\" LIMIT $5::integer) " +
      std::string{AUTH_REPLY} + " UNION ALL " + std::string{TARGET_REPLY};
  auto result = query(connection, sql, values);
  auto rows = read_rows(result.get(), auth, limit + 1);
  if (!rows) return std::unexpected(rows.error());
  SurfacePage page{.caller = std::move(rows->caller),
                   .targets = std::move(rows->targets),
                   .next_position = std::nullopt};
  // SQL ORDER BY+LIMIT selects the exact prefix. UNION ALL result order itself
  // is not a SQL guarantee, so restore numeric/byte ordering before lookahead.
  std::ranges::sort(page.targets, [](const auto& left, const auto& right) {
    const auto& a = *left.logical_install_order;
    const auto& b = *right.logical_install_order;
    if (a.size() != b.size()) return a.size() < b.size();
    if (a != b) return a < b;
    if (left.application_id != right.application_id)
      return left.application_id < right.application_id;
    return left.declaration.id < right.declaration.id;
  });
  if (page.targets.size() > limit) {
    page.targets.pop_back();
    const auto& last = page.targets.back();
    page.next_position =
        Position{.logical_install_order = last.logical_install_order,
                 .application_id = last.application_id,
                 .panel_id = last.declaration.id};
  }
  return page;
} catch (const std::exception&) {
  return std::unexpected(Error::UNAVAILABLE);
}
} // namespace

auto catalog_float_prefix(PGconn& connection, const auth::AuthContext& auth,
                          std::string_view content_type, std::size_t limit,
                          const std::optional<Position>& after)
    -> std::expected<SurfacePage, Error> {
  return catalog_float_prefix_impl(connection, auth, content_type, limit,
                                   after);
}
auto catalog_float_prefix(PGconn& connection, const auth::AuthContext& auth,
                          std::nullopt_t, std::size_t limit,
                          const std::optional<Position>& after)
    -> std::expected<SurfacePage, Error> {
  return catalog_float_prefix_impl(connection, auth, std::nullopt, limit,
                                   after);
}

auto catalog_surface_prefix(PGconn& connection, const auth::AuthContext& auth,
                            SurfaceKind kind, std::size_t limit,
                            const std::optional<Position>& after)
    -> std::expected<SurfacePage, Error> try {
  if (!valid_auth(auth)) return std::unexpected(Error::UNAUTHENTICATED);
  if ((kind != SurfaceKind::PRIMARY && kind != SurfaceKind::TRAY) ||
      limit == 0 || limit > 64 ||
      (after && (after->logical_install_order ||
                 !identifier(after->application_id, false) ||
                 !identifier(after->panel_id, true))))
    return std::unexpected(Error::INVALID);
  auto values = auth_values(auth);
  values.emplace_back(kind_name(kind));
  values.push_back(std::to_string(limit + 1));
  std::string cursor;
  if (after) {
    values.push_back(after->application_id);
    values.push_back(after->panel_id);
    cursor = "AND (p.name COLLATE \"C\",pn.panel_id COLLATE \"C\") > "
             "($6 COLLATE \"C\",$7 COLLATE \"C\") ";
  }
  const auto sql = auth_cte(auth) + ", selected AS (" + std::string{ELIGIBLE} +
                   "AND pn.panel_type=$4 " + cursor +
                   "ORDER BY p.name COLLATE \"C\",pn.panel_id COLLATE \"C\" "
                   "LIMIT $5::integer) " +
                   std::string{AUTH_REPLY} + " UNION ALL " +
                   std::string{TARGET_REPLY};
  auto result = query(connection, sql, values);
  auto rows = read_rows(result.get(), auth, limit + 1);
  if (!rows) return std::unexpected(rows.error());
  SurfacePage page{.caller = std::move(rows->caller),
                   .targets = std::move(rows->targets),
                   .next_position = std::nullopt};
  // UNION ALL does not promise the selected prefix's output order.
  std::ranges::sort(page.targets, [](const auto& left, const auto& right) {
    if (left.application_id != right.application_id)
      return left.application_id < right.application_id;
    return left.declaration.id < right.declaration.id;
  });
  if (page.targets.size() > limit) {
    page.targets.pop_back();
    const auto& last = page.targets.back();
    page.next_position = Position{.logical_install_order = std::nullopt,
                                  .application_id = last.application_id,
                                  .panel_id = last.declaration.id};
  }
  return page;
} catch (const std::exception&) {
  return std::unexpected(Error::UNAVAILABLE);
}

} // namespace plinth::frontend::authority
