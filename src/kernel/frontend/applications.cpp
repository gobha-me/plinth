#include "kernel/frontend/applications.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/orm/Result.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace plinth::frontend {
namespace {

constexpr std::string_view DISCOVERY_SQL =
    "WITH effective_rules AS ("
    "  SELECT DISTINCT r.rule "
    "  FROM plinth.rbac_rules r "
    "  JOIN plinth.group_rules gr ON gr.rule_id = r.id "
    "  JOIN plinth.groups g ON g.id = gr.group_id "
    "  LEFT JOIN plinth.group_members gm ON gm.group_id = g.id "
    "  WHERE r.orphaned_at IS NULL "
    "    AND (g.name = 'everyone' OR gm.user_id = $1::uuid)"
    "), effective_json AS ("
    "  SELECT COALESCE(jsonb_agg(rule ORDER BY rule), '[]'::jsonb) AS rules "
    "  FROM effective_rules"
    ") "
    "SELECT p.id::text, p.name, p.version, p.manifest_json::text, "
    "       pn.panel_id, pn.declaration::text, effective_json.rules::text "
    "FROM plinth.packages p "
    "JOIN plinth.panels pn ON pn.package_id = p.id "
    "JOIN plinth.rbac_rules panel_rule "
    "  ON panel_rule.rule = pn.declaration->>'rbac_rule' "
    " AND panel_rule.extension_name = p.name "
    " AND panel_rule.orphaned_at IS NULL "
    "CROSS JOIN effective_json "
    "WHERE p.application_ready "
    "  AND p.state IN ('ACTIVE', 'ACTIVE_FLAGGED') "
    "  AND pn.panel_type = 'primary'";

struct RawRow {
  std::string generation;
  std::string application_id;
  std::string version;
  std::string manifest;
  std::string panel_id;
  std::string declaration;
  std::string effective_rules;
};

struct Panel {
  std::string id;
  std::string title;
  std::string icon;
  std::string module_url;
  std::int64_t order{0};
};

struct Application {
  std::string id;
  std::string generation;
  std::string version;
  std::string title;
  std::string description;
  std::string icon;
  std::vector<Panel> panels;
};

auto valid_icon(std::string_view value) -> bool {
  if (value.empty() || value.size() > 64 || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(value.substr(1), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

auto valid_rule(std::string_view value) -> bool {
  if (value.empty()) {
    return false;
  }
  std::size_t segments = 0;
  std::size_t pos = 0;
  while (pos < value.size()) {
    if (value[pos] < 'a' || value[pos] > 'z') {
      return false;
    }
    ++pos;
    while (pos < value.size() && value[pos] != '.') {
      const char c = value[pos++];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
        return false;
      }
    }
    ++segments;
    if (pos < value.size()) {
      ++pos;
      if (pos == value.size()) {
        return false;
      }
    }
  }
  return segments >= 2 && segments <= 5;
}

// PostgreSQL JSONB guarantees valid UTF-8. Counting non-continuation bytes is
// therefore the Unicode-scalar count needed for the persisted metadata caps.
auto scalar_count(std::string_view value) -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(value, [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
  }));
}

auto bounded_string(const nlohmann::json& value, std::size_t max_scalars)
    -> std::optional<std::string> {
  if (!value.is_string()) {
    return std::nullopt;
  }
  auto result = value.get<std::string>();
  if (result.empty() || scalar_count(result) > max_scalars) {
    return std::nullopt;
  }
  return result;
}

auto title_fallback(std::string_view id, bool underscores) -> std::string {
  std::string result{id};
  bool capitalize = true;
  for (char& c : result) {
    if (c == '-' || (underscores && c == '_')) {
      c = ' ';
      capitalize = true;
      continue;
    }
    if (capitalize && c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - ('a' - 'A'));
    }
    capitalize = (c == ' ');
  }
  return result;
}

auto encode_segment(std::string_view value) -> std::string {
  constexpr std::string_view HEX = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(HEX[c >> 4U]);
      encoded.push_back(HEX[c & 0x0FU]);
    }
  }
  return encoded;
}

auto encoded_client_path(std::string_view path) -> std::optional<std::string> {
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  std::string result;
  std::size_t begin = 0;
  while (begin < path.size()) {
    const auto slash = path.find('/', begin);
    const auto end = slash == std::string_view::npos ? path.size() : slash;
    const auto segment = path.substr(begin, end - begin);
    if (segment.empty() || segment == "." || segment == "..") {
      return std::nullopt;
    }
    if (!result.empty()) {
      result.push_back('/');
    }
    result += encode_segment(segment);
    if (slash == std::string_view::npos) {
      break;
    }
    begin = slash + 1;
  }
  return result;
}

auto parse_json(std::string_view text) -> std::optional<nlohmann::json> {
  auto parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::nullopt;
  }
  return parsed;
}

auto parse_rules(std::string_view text)
    -> std::optional<std::unordered_set<std::string>> {
  auto parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) {
    return std::nullopt;
  }
  std::unordered_set<std::string> result;
  for (const auto& value : parsed) {
    if (!value.is_string()) {
      return std::nullopt;
    }
    result.insert(value.get<std::string>());
  }
  return result;
}

auto materialize(const std::vector<RawRow>& rows)
    -> std::optional<std::vector<Application>> {
  std::vector<Application> applications;
  for (const auto& row : rows) {
    if (capabilities::drain::is_application_blocked(row.application_id)) {
      continue;
    }
    const auto manifest = parse_json(row.manifest);
    const auto declaration = parse_json(row.declaration);
    const auto rules = parse_rules(row.effective_rules);
    if (!manifest || !declaration || !rules) {
      spdlog::warn("frontend discovery omitted malformed stored row app={} "
                   "panel={}",
                   row.application_id, row.panel_id);
      continue;
    }

    const auto rule_it = declaration->find("rbac_rule");
    const auto path_it = declaration->find("client_path");
    if (rule_it == declaration->end() || !rule_it->is_string() ||
        path_it == declaration->end() || !path_it->is_string()) {
      spdlog::warn("frontend discovery omitted malformed stored row app={} "
                   "panel={}",
                   row.application_id, row.panel_id);
      continue;
    }
    const auto rule = rule_it->get<std::string>();
    const auto encoded_path = encoded_client_path(path_it->get<std::string>());
    if (!valid_rule(rule) || !encoded_path ||
        (!rules->contains("kernel.admin") && !rules->contains(rule))) {
      if (!valid_rule(rule) || !encoded_path) {
        spdlog::warn("frontend discovery omitted malformed stored row app={} "
                     "panel={}",
                     row.application_id, row.panel_id);
      }
      continue;
    }

    std::int64_t order = 0;
    if (const auto it = declaration->find("order"); it != declaration->end()) {
      if (!it->is_number_integer()) {
        spdlog::warn("frontend discovery omitted malformed stored row app={} "
                     "panel={}",
                     row.application_id, row.panel_id);
        continue;
      }
      order = it->get<std::int64_t>();
      if (order < 0 || order > 10000) {
        spdlog::warn("frontend discovery omitted malformed stored row app={} "
                     "panel={}",
                     row.application_id, row.panel_id);
        continue;
      }
    }

    std::string app_title = title_fallback(row.application_id, false);
    if (const auto it = manifest->find("display_name"); it != manifest->end()) {
      if (auto value = bounded_string(*it, 128)) {
        app_title = std::move(*value);
      }
    }
    std::string app_icon;
    if (const auto it = manifest->find("icon");
        it != manifest->end() && it->is_string()) {
      auto candidate = it->get<std::string>();
      if (valid_icon(candidate)) {
        app_icon = std::move(candidate);
      }
    }
    std::string description;
    if (const auto it = manifest->find("description");
        it != manifest->end() && it->is_string()) {
      description = it->get<std::string>();
    }

    auto app = std::ranges::find_if(applications, [&](const Application& v) {
      return v.id == row.application_id && v.generation == row.generation;
    });
    if (app == applications.end()) {
      applications.push_back({.id = row.application_id,
                              .generation = row.generation,
                              .version = row.version,
                              .title = std::move(app_title),
                              .description = std::move(description),
                              .icon = std::move(app_icon),
                              .panels = {}});
      app = std::prev(applications.end());
    }

    std::string panel_title = title_fallback(row.panel_id, true);
    if (const auto it = declaration->find("title"); it != declaration->end()) {
      auto value = bounded_string(*it, 256);
      if (!value) {
        spdlog::warn("frontend discovery omitted malformed stored row app={} "
                     "panel={}",
                     row.application_id, row.panel_id);
        continue;
      }
      panel_title = std::move(*value);
    }
    std::string panel_icon = app->icon;
    if (const auto it = declaration->find("icon"); it != declaration->end()) {
      if (!it->is_string() || !valid_icon(it->get_ref<const std::string&>())) {
        spdlog::warn("frontend discovery omitted malformed stored row app={} "
                     "panel={}",
                     row.application_id, row.panel_id);
        continue;
      }
      panel_icon = it->get<std::string>();
    }
    app->panels.push_back(
        {.id = row.panel_id,
         .title = std::move(panel_title),
         .icon = std::move(panel_icon),
         .module_url = "/ext/" + encode_segment(row.application_id) + "/" +
                       encode_segment(row.version) + "/panels/" + *encoded_path,
         .order = order});
  }

  std::erase_if(applications,
                [](const Application& app) { return app.panels.empty(); });
  std::ranges::sort(applications,
                    [](const Application& lhs, const Application& rhs) {
                      return lhs.id < rhs.id;
                    });
  for (auto& app : applications) {
    std::ranges::sort(app.panels, [](const Panel& lhs, const Panel& rhs) {
      return lhs.order < rhs.order ||
             (lhs.order == rhs.order && lhs.id < rhs.id);
    });
  }
  return applications;
}

auto response_headers(const drogon::HttpResponsePtr& response) -> void {
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Vary", "Cookie, Authorization");
}

auto make_json_response(drogon::HttpStatusCode status,
                        const nlohmann::json& body) -> drogon::HttpResponsePtr {
  auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(status);
  response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  response->setBody(body.dump());
  response_headers(response);
  return response;
}

auto make_unavailable() -> drogon::HttpResponsePtr {
  return make_json_response(
      drogon::k503ServiceUnavailable,
      {{"error", "service_unavailable"},
       {"message", "Application discovery is temporarily unavailable"}});
}

auto make_unauthenticated() -> drogon::HttpResponsePtr {
  return make_json_response(drogon::k401Unauthorized,
                            {{"error", "not_authenticated"},
                             {"message", "No authentication token provided"}});
}

auto make_success(const std::vector<RawRow>& rows) -> drogon::HttpResponsePtr {
  auto applications = materialize(rows);
  if (!applications) {
    return make_unavailable();
  }
  nlohmann::json values = nlohmann::json::array();
  for (const auto& app : *applications) {
    nlohmann::json panels = nlohmann::json::array();
    for (const auto& panel : app.panels) {
      panels.push_back({{"id", panel.id},
                        {"title", panel.title},
                        {"icon", panel.icon},
                        {"module_url", panel.module_url}});
    }
    values.push_back({{"id", app.id},
                      {"generation", app.generation},
                      {"version", app.version},
                      {"title", app.title},
                      {"description", app.description},
                      {"icon", app.icon},
                      {"panels", std::move(panels)}});
  }
  return make_json_response(
      drogon::k200OK,
      {{"schema_version", 1}, {"applications", std::move(values)}});
}

auto auth_user(const drogon::HttpRequestPtr& request)
    -> std::optional<std::string> {
  auto context = auth::get_auth_context(request);
  if (!context || context->user_id.empty()) {
    return std::nullopt;
  }
  return context->user_id;
}

auto rows_from_result(const drogon::orm::Result& result)
    -> std::vector<RawRow> {
  std::vector<RawRow> rows;
  rows.reserve(result.size());
  for (const auto& row : result) {
    rows.push_back({.generation = row[0].as<std::string>(),
                    .application_id = row[1].as<std::string>(),
                    .version = row[2].as<std::string>(),
                    .manifest = row[3].as<std::string>(),
                    .panel_id = row[4].as<std::string>(),
                    .declaration = row[5].as<std::string>(),
                    .effective_rules = row[6].as<std::string>()});
  }
  return rows;
}

auto handle_applications(
    const drogon::HttpRequestPtr& request,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) -> void {
  const auto user_id = auth_user(request);
  if (!user_id) {
    std::move(callback)(make_unauthenticated());
    return;
  }
  auto shared_callback =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
          std::move(callback));
  try {
    drogon::app().getDbClient()->execSqlAsync(
        std::string{DISCOVERY_SQL},
        [shared_callback](const drogon::orm::Result& result) {
          try {
            (*shared_callback)(make_success(rows_from_result(result)));
          } catch (const std::exception& error) {
            spdlog::error("frontend discovery result failed: {}", error.what());
            (*shared_callback)(make_unavailable());
          }
        },
        [shared_callback](const drogon::orm::DrogonDbException& error) {
          spdlog::error("frontend discovery query failed: {}",
                        error.base().what());
          (*shared_callback)(make_unavailable());
        },
        *user_id);
  } catch (const std::exception& error) {
    spdlog::error("frontend discovery query start failed: {}", error.what());
    (*shared_callback)(make_unavailable());
  }
}

auto query_sync(const Config::Database& db, std::string_view user_id)
    -> std::optional<std::vector<RawRow>> {
  auto connection =
      plinth::db::connect(plinth::db::connection_info(db).c_str());
  if (PQstatus(connection) != CONNECTION_OK) {
    spdlog::error("frontend discovery PG connect failed: {}",
                  PQerrorMessage(connection));
    PQfinish(connection);
    return std::nullopt;
  }
  auto cleanup = [](PGconn* value) { PQfinish(value); };
  std::unique_ptr<PGconn, decltype(cleanup)> guard(connection, cleanup);
  const std::string user{user_id};
  const std::array<const char*, 1> values{user.c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> result(
      plinth::db::exec_params(connection, DISCOVERY_SQL.data(), 1, nullptr,
                              values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
    spdlog::error("frontend discovery SELECT failed: {}",
                  PQresultErrorMessage(result.get()));
    return std::nullopt;
  }
  std::vector<RawRow> rows;
  rows.reserve(static_cast<std::size_t>(PQntuples(result.get())));
  for (int row = 0; row < PQntuples(result.get()); ++row) {
    rows.push_back({.generation = PQgetvalue(result.get(), row, 0),
                    .application_id = PQgetvalue(result.get(), row, 1),
                    .version = PQgetvalue(result.get(), row, 2),
                    .manifest = PQgetvalue(result.get(), row, 3),
                    .panel_id = PQgetvalue(result.get(), row, 4),
                    .declaration = PQgetvalue(result.get(), row, 5),
                    .effective_rules = PQgetvalue(result.get(), row, 6)});
  }
  return rows;
}

} // namespace

auto register_application_routes() -> void {
  drogon::app().registerHandler(
      "/api/frontend/applications",
      [](const drogon::HttpRequestPtr& request,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_applications(request, std::move(callback));
      },
      {drogon::Get, "plinth::auth::SessionFilter"});
  spdlog::info("frontend: registered GET /api/frontend/applications");
}

namespace test_seam {

auto dispatch_applications(
    const drogon::HttpRequestPtr& request,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    const Config::Database& db) -> void {
  const auto user_id = auth_user(request);
  if (!user_id) {
    std::move(callback)(make_unauthenticated());
    return;
  }
  auto rows = query_sync(db, *user_id);
  std::move(callback)(rows ? make_success(*rows) : make_unavailable());
}

} // namespace test_seam
} // namespace plinth::frontend
