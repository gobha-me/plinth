#include "kernel/auth/middleware.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/frontend/applications.hpp"
#include "kernel/groups/handlers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

auto pg_available() -> bool {
  return std::getenv("PLINTH_PG_HOST") != nullptr;
}

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (const auto* value = std::getenv("PLINTH_PG_HOST")) {
    db.host = value;
  }
  if (const auto* value = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<std::uint16_t>(std::stoi(value));
  }
  if (const auto* value = std::getenv("PLINTH_PG_USER")) {
    db.user = value;
  }
  if (const auto* value = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = value;
  }
  if (const auto* value = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = value;
  }
  return db;
}

auto connection_info(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

auto execute(const plinth::Config::Database& db, const std::string& sql)
    -> void {
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(
      PQconnectdb(connection_info(db).c_str()), PQfinish);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  std::unique_ptr<PGresult, decltype(&PQclear)> result(
      PQexec(connection.get(), sql.c_str()), PQclear);
  REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
}

struct Scratch {
  plinth::Config::Database db{pg_config()};

  Scratch() {
    plinth::db::bootstrap_schema(
        db, std::string{CMAKE_SOURCE_DIR} + "/migrations", true);
    plinth::groups::bootstrap_groups(db);
  }
};

constexpr std::string_view USER_ID = "10000000-0000-4000-8000-000000000001";

auto request_for(std::string_view user_id = USER_ID) -> drogon::HttpRequestPtr {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(drogon::Get);
  request->setPath("/api/frontend/applications");
  if (!user_id.empty()) {
    auto attributes = request->attributes();
    attributes->insert(plinth::auth::ATTR_USER_ID, std::string{user_id});
    attributes->insert(plinth::auth::ATTR_USERNAME, std::string{"launcher"});
    attributes->insert(plinth::auth::ATTR_AUTH_TYPE, std::string{"session"});
    attributes->insert(plinth::auth::ATTR_SESSION_ID,
                       std::string{"20000000-0000-4000-8000-000000000001"});
    attributes->insert(plinth::auth::ATTR_PAT_ID, std::string{});
    attributes->insert(plinth::auth::ATTR_TOKEN_HASH, std::string{"fixture"});
  }
  return request;
}

struct Captured {
  drogon::HttpStatusCode status{drogon::k500InternalServerError};
  drogon::ContentType content_type{drogon::CT_NONE};
  std::string body;
  std::string cache_control;
  std::string vary;
};

auto dispatch(const plinth::Config::Database& db,
              const drogon::HttpRequestPtr& request) -> Captured {
  Captured captured;
  plinth::frontend::test_seam::dispatch_applications(
      request,
      [&captured](const drogon::HttpResponsePtr& response) {
        REQUIRE(response);
        captured.status = response->statusCode();
        captured.content_type = response->getContentType();
        captured.body = std::string{response->body()};
        captured.cache_control = response->getHeader("Cache-Control");
        captured.vary = response->getHeader("Vary");
      },
      db);
  return captured;
}

auto seed_user(const plinth::Config::Database& db) -> void {
  execute(db, "INSERT INTO plinth.users(id, username, password_hash) VALUES "
              "('10000000-0000-4000-8000-000000000001', 'launcher', 'unused')");
}

auto seed_rule(const plinth::Config::Database& db, std::string_view rule,
               std::string_view package, std::string_view group) -> void {
  execute(db, "INSERT INTO plinth.rbac_rules(rule, namespace, description, "
              "extension_name) VALUES ('" +
                  std::string{rule} + "', '" + std::string{package} +
                  "', 'fixture', '" + std::string{package} + "')");
  execute(db, "INSERT INTO plinth.group_rules(group_id, rule_id) "
              "SELECT g.id, r.id FROM plinth.groups g, plinth.rbac_rules r "
              "WHERE g.name='" +
                  std::string{group} + "' AND r.rule='" + std::string{rule} +
                  "'");
}

auto add_user_to_group(const plinth::Config::Database& db,
                       std::string_view group) -> void {
  execute(db, "INSERT INTO plinth.group_members(group_id, user_id) "
              "SELECT id, '10000000-0000-4000-8000-000000000001'::uuid "
              "FROM plinth.groups WHERE name='" +
                  std::string{group} + "'");
}

auto seed_package(const plinth::Config::Database& db, std::string_view name,
                  std::string_view version, std::string_view state, bool ready,
                  std::string_view manifest) -> void {
  execute(db,
          "INSERT INTO plinth.packages(name, version, state, provenance, "
          "manifest_json, entry_point, manifest_checksum, application_ready) "
          "VALUES ('" +
              std::string{name} + "', '" + std::string{version} + "', '" +
              std::string{state} + "', 'user', '" + std::string{manifest} +
              "'::jsonb, 'server/main.js', 'fixture', " +
              (ready ? "TRUE" : "FALSE") + ")");
}

auto seed_panel(const plinth::Config::Database& db, std::string_view package,
                std::string_view panel_id, std::string_view declaration)
    -> void {
  execute(db, "INSERT INTO plinth.panels(package_id, panel_id, panel_type, "
              "declaration) SELECT id, '" +
                  std::string{panel_id} + "', 'primary', '" +
                  std::string{declaration} +
                  "'::jsonb FROM plinth.packages WHERE name='" +
                  std::string{package} + "'");
}

} // namespace

TEST_CASE("application discovery rejects a missing authentication context",
          "[frontend][applications][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  Scratch scratch;
  const auto response = dispatch(scratch.db, request_for(""));
  REQUIRE(response.status == drogon::k401Unauthorized);
  REQUIRE(nlohmann::json::parse(response.body) ==
          nlohmann::json{{"error", "not_authenticated"},
                         {"message", "No authentication token provided"}});
  REQUIRE(response.cache_control == "no-store");
  REQUIRE(response.vary == "Cookie, Authorization");
}

TEST_CASE("application discovery returns an authenticated empty catalog",
          "[frontend][applications][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  Scratch scratch;
  seed_user(scratch.db);
  const auto response = dispatch(scratch.db, request_for());
  REQUIRE(response.status == drogon::k200OK);
  REQUIRE(response.content_type == drogon::CT_APPLICATION_JSON);
  REQUIRE(nlohmann::json::parse(response.body) ==
          nlohmann::json{{"schema_version", 1},
                         {"applications", nlohmann::json::array()}});
  REQUIRE(response.cache_control == "no-store");
  REQUIRE(response.vary == "Cookie, Authorization");
}

TEST_CASE("application discovery filters, validates, orders, and encodes",
          "[frontend][applications][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  Scratch scratch;
  seed_user(scratch.db);
  execute(scratch.db, "INSERT INTO plinth.groups(name, description) VALUES "
                      "('launcher-team', 'fixture')");
  add_user_to_group(scratch.db, "launcher-team");
  seed_rule(scratch.db, "notes.ui.editor", "notes", "launcher-team");
  seed_rule(scratch.db, "notes.ui.early", "notes", "launcher-team");
  seed_rule(scratch.db, "notes.ui.secret", "notes", "admin");
  seed_rule(scratch.db, "tasks.ui.overview", "tasks", "everyone");
  seed_rule(scratch.db, "flagged.ui.main", "flagged", "everyone");

  seed_package(
      scratch.db, "tasks", "2.0.0", "ACTIVE", true,
      R"({"name":"tasks","version":"2.0.0","description":"Tasks","icon":"NOT VALID"})");
  seed_panel(
      scratch.db, "tasks", "overview_panel",
      R"({"client_path":"overview.js","rbac_rule":"tasks.ui.overview"})");
  seed_panel(scratch.db, "tasks", "malformed",
             R"({"client_path":"bad.js","order":"first"})");

  seed_package(
      scratch.db, "notes", "1.2.3", "ACTIVE", true,
      R"({"name":"notes","version":"1.2.3","description":"Markdown notes","display_name":"Notes","icon":"edit-3"})");
  seed_panel(
      scratch.db, "notes", "editor",
      R"({"client_path":"nested/editor file.js","title":"Editor","icon":"edit-3","rbac_rule":"notes.ui.editor","order":10,"future":{"kept":true}})");
  seed_panel(
      scratch.db, "notes", "alpha",
      R"({"client_path":"alpha.js","rbac_rule":"notes.ui.editor","order":10})");
  seed_panel(
      scratch.db, "notes", "early",
      R"({"client_path":"early.js","rbac_rule":"notes.ui.early","order":1})");
  seed_panel(
      scratch.db, "notes", "secret",
      R"({"client_path":"secret.js","title":"DENIED-METADATA","rbac_rule":"notes.ui.secret","order":0})");

  seed_package(
      scratch.db, "flagged", "3.0.0", "ACTIVE_FLAGGED", true,
      R"({"name":"flagged","version":"3.0.0","description":"Flagged"})");
  seed_panel(scratch.db, "flagged", "main",
             R"({"client_path":"main.js","rbac_rule":"flagged.ui.main"})");
  seed_package(
      scratch.db, "unready", "1.0.0", "ACTIVE", false,
      R"({"name":"unready","version":"1.0.0","description":"Hidden"})");
  seed_panel(scratch.db, "unready", "main",
             R"({"client_path":"main.js","rbac_rule":"tasks.ui.overview"})");
  seed_package(
      scratch.db, "disabled", "1.0.0", "DISABLED", false,
      R"({"name":"disabled","version":"1.0.0","description":"Hidden"})");
  seed_panel(scratch.db, "disabled", "main",
             R"({"client_path":"main.js","rbac_rule":"tasks.ui.overview"})");
  seed_package(
      scratch.db, "foreign", "1.0.0", "ACTIVE", true,
      R"({"name":"foreign","version":"1.0.0","description":"Hidden"})");
  seed_panel(scratch.db, "foreign", "main",
             R"({"client_path":"main.js","rbac_rule":"tasks.ui.overview"})");

  const auto response = dispatch(scratch.db, request_for());
  REQUIRE(response.status == drogon::k200OK);
  const auto body = nlohmann::json::parse(response.body);
  REQUIRE(body["schema_version"] == 1);
  REQUIRE(body["applications"].size() == 3);
  REQUIRE(body["applications"][0]["id"] == "flagged");
  REQUIRE(body["applications"][1]["id"] == "notes");
  REQUIRE(body["applications"][2]["id"] == "tasks");

  const auto& notes = body["applications"][1];
  REQUIRE(notes.size() == 7);
  REQUIRE(notes["title"] == "Notes");
  REQUIRE(notes["description"] == "Markdown notes");
  REQUIRE(notes["icon"] == "edit-3");
  REQUIRE(notes["generation"].is_string());
  REQUIRE(notes["panels"].size() == 3);
  REQUIRE(notes["panels"][0]["id"] == "early");
  REQUIRE(notes["panels"][1]["id"] == "alpha");
  REQUIRE(notes["panels"][2]["id"] == "editor");
  REQUIRE(notes["panels"][2]["module_url"] ==
          "/ext/notes/1.2.3/panels/nested/editor%20file.js");
  REQUIRE(notes["panels"][2].size() == 4);

  const auto& tasks = body["applications"][2];
  REQUIRE(tasks["title"] == "Tasks");
  REQUIRE(tasks["icon"] == "");
  REQUIRE(tasks["panels"].size() == 1);
  REQUIRE(tasks["panels"][0]["title"] == "Overview Panel");
  REQUIRE(tasks["panels"][0]["icon"] == "");
  REQUIRE(response.body.find("DENIED-METADATA") == std::string::npos);
  REQUIRE(response.body.find("rbac_rule") == std::string::npos);
  REQUIRE(response.cache_control == "no-store");
  REQUIRE(response.vary == "Cookie, Authorization");
}

TEST_CASE("application discovery honors the kernel admin universal match",
          "[frontend][applications][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  Scratch scratch;
  seed_user(scratch.db);
  add_user_to_group(scratch.db, "admin");
  seed_rule(scratch.db, "private.ui.main", "private", "everyone");
  execute(scratch.db,
          "DELETE FROM plinth.group_rules gr USING plinth.rbac_rules r "
          "WHERE gr.rule_id=r.id AND r.rule='private.ui.main'");
  seed_package(
      scratch.db, "private", "1.0.0", "ACTIVE", true,
      R"({"name":"private","version":"1.0.0","description":"Private"})");
  seed_panel(scratch.db, "private", "main",
             R"({"client_path":"main.js","rbac_rule":"private.ui.main"})");

  const auto body =
      nlohmann::json::parse(dispatch(scratch.db, request_for()).body);
  REQUIRE(body["applications"].size() == 1);
  REQUIRE(body["applications"][0]["id"] == "private");
}

TEST_CASE("application discovery omits lifecycle-blocked generations",
          "[frontend][applications][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  Scratch scratch;
  seed_user(scratch.db);
  seed_rule(scratch.db, "blocked.ui.main", "blocked", "everyone");
  seed_package(
      scratch.db, "blocked", "1.0.0", "ACTIVE", true,
      R"({"name":"blocked","version":"1.0.0","description":"Blocked"})");
  seed_panel(scratch.db, "blocked", "main",
             R"({"client_path":"main.js","rbac_rule":"blocked.ui.main"})");

  auto drain = plinth::capabilities::drain::begin_drain("blocked");
  (void)drain;
  plinth::capabilities::drain::block_application("blocked");
  const auto body =
      nlohmann::json::parse(dispatch(scratch.db, request_for()).body);
  REQUIRE(body["applications"].empty());
  plinth::capabilities::drain::end_drain("blocked");
}

TEST_CASE("application discovery reports query failure without detail",
          "[frontend][applications][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  Scratch scratch;
  seed_user(scratch.db);
  execute(scratch.db, "ALTER TABLE plinth.panels RENAME TO panels_unavailable");
  const auto response = dispatch(scratch.db, request_for());
  execute(scratch.db, "ALTER TABLE plinth.panels_unavailable RENAME TO panels");

  REQUIRE(response.status == drogon::k503ServiceUnavailable);
  REQUIRE(nlohmann::json::parse(response.body) ==
          nlohmann::json{
              {"error", "service_unavailable"},
              {"message", "Application discovery is temporarily unavailable"}});
  REQUIRE(response.body.find("panels_unavailable") == std::string::npos);
  REQUIRE(response.cache_control == "no-store");
  REQUIRE(response.vary == "Cookie, Authorization");
}
