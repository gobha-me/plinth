// SPDX-License-Identifier: MIT
//
// ICD-0.6.1/0.6.2/0.6.3 deferred JS-dispatch contract backfill.

#include "kernel/auth/middleware.hpp"
#include "kernel/cap/api_cap.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/shell/firstboot.hpp"

#include "../js/async_bridge_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

auto conninfo_of(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

class PgConnection {
 public:
  explicit PgConnection(const plinth::Config::Database& db)
      : conn(PQconnectdb(conninfo_of(db).c_str()), &PQfinish) {
    REQUIRE(PQstatus(conn.get()) == CONNECTION_OK);
  }

  auto exec(std::string_view sql) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    auto result = std::unique_ptr<PGresult, decltype(&PQclear)>(
        PQexec(conn.get(), std::string{sql}.c_str()), &PQclear);
    REQUIRE((PQresultStatus(result.get()) == PGRES_TUPLES_OK ||
             PQresultStatus(result.get()) == PGRES_COMMAND_OK));
    return result;
  }

  auto exec_params(std::string_view sql,
                   const std::vector<std::string>& params) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& param : params) {
      values.push_back(param.c_str());
    }
    auto result = std::unique_ptr<PGresult, decltype(&PQclear)>(
        PQexecParams(conn.get(), std::string{sql}.c_str(),
                     static_cast<int>(values.size()), nullptr, values.data(),
                     nullptr, nullptr, 0),
        &PQclear);
    REQUIRE((PQresultStatus(result.get()) == PGRES_TUPLES_OK ||
             PQresultStatus(result.get()) == PGRES_COMMAND_OK));
    return result;
  }

 private:
  std::unique_ptr<PGconn, decltype(&PQfinish)> conn;
};

auto drop_schemas(const plinth::Config::Database& db) -> void {
  PgConnection pg(db);
  auto schemas = pg.exec("SELECT quote_ident(nspname) FROM pg_namespace "
                         "WHERE nspname LIKE 'ext\\_%' ESCAPE '\\'");
  for (int row = 0; row < PQntuples(schemas.get()); ++row) {
    pg.exec("DROP SCHEMA IF EXISTS " +
            std::string{PQgetvalue(schemas.get(), row, 0)} + " CASCADE");
  }
  pg.exec("DROP SCHEMA IF EXISTS plinth CASCADE");
}

std::atomic<std::uint64_t> scratch_counter{0};

class ShellDispatchFixture {
 public:
  explicit ShellDispatchFixture(std::string_view test_name) {
    plinth::async_bridge_test::ensure_drogon_with_db_running();
    cfg = plinth::async_bridge_test::test_config();

    auto suffix = std::to_string(::getpid()) + "_" +
                  std::to_string(scratch_counter.fetch_add(1));
    root = fs::temp_directory_path() / ("plinth_shell_dispatch_" + suffix);
    fs::create_directories(root / "data");
    fs::create_directories(root / "staging");

    drop_schemas(cfg.db);
    plinth::db::bootstrap_schema(cfg.db, cfg.migrations_dir, true);
    plinth::groups::bootstrap_groups(cfg.db);

    plinth::packages::InstallerContext installer{
        .db = cfg.db,
        .caller_user_id = "",
        .data_dir = root / "data",
        .staging_dir = root / "staging",
        .max_package_size_bytes = 50ULL * 1024ULL * 1024ULL,
    };
    cfg.packages_data_dir = installer.data_dir.string();
    cfg.packages_staging_dir = installer.staging_dir.string();
    cfg.shell.bundle_path =
        std::string{CMAKE_BINARY_DIR} + "/share/plinth/bundled";
    REQUIRE(plinth::shell::ensure_bundled_shell_installed(cfg, installer)
                .has_value());

    plinth::capabilities::init_resolver(cfg.db);
    plinth::extensions::init_registry(cfg);
    username = std::string{test_name};
    user_id = add_user(username);
  }

  ~ShellDispatchFixture() {
    for (const auto& key : successful_set_keys) {
      (void)wait_for_audit(key, 2s);
    }
    (void)plinth::extensions::shutdown_registry();
    plinth::capabilities::clear_resolver_for_test();
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  ShellDispatchFixture(const ShellDispatchFixture&) = delete;
  auto operator=(const ShellDispatchFixture&) -> ShellDispatchFixture& = delete;
  ShellDispatchFixture(ShellDispatchFixture&&) = delete;
  auto operator=(ShellDispatchFixture&&) -> ShellDispatchFixture& = delete;

  auto context(std::string id = {},
               std::vector<std::string> rules = {"shell.preferences.read",
                                                 "shell.preferences.write",
                                                 "shell.audit.emit"}) const
      -> plinth::capabilities::UserContext {
    return {
        .user_id = id.empty() ? user_id : id,
        .username = username,
        .auth_type = "session",
        .effective_rules = std::move(rules),
        .session_id = "",
        .ip_address = "127.0.0.1",
    };
  }

  auto add_user(std::string_view name) const -> std::string {
    PgConnection pg(cfg.db);
    auto result =
        pg.exec_params("INSERT INTO plinth.users (username, password_hash) "
                       "VALUES ($1, 'unused') RETURNING id::text",
                       {std::string{name}});
    return PQgetvalue(result.get(), 0, 0);
  }

  auto call(std::string_view function, const Json::Value& args,
            const plinth::capabilities::UserContext* caller = nullptr,
            std::string* detail_code = nullptr)
      -> plinth::capabilities::ResolveResult {
    auto owned_context = context();
    const auto& selected_context = caller == nullptr ? owned_context : *caller;
    plinth::capabilities::CapabilityCall request{
        .signature = "shell:1:" + std::string{function},
        .args = args,
        .call_depth = 0,
    };
    std::string detail_message;
    auto result = drogon::sync_wait(plinth::capabilities::call_capability_async(
        request, selected_context, detail_code, &detail_message));
    if (result.has_value() && function == "preferences.set" &&
        args.isObject() && args["key"].isString()) {
      successful_set_keys.push_back(args["key"].asString());
    }
    return result;
  }

  auto http_call(std::string_view function, const Json::Value& args)
      -> drogon::HttpResponsePtr {
    make_admin(user_id);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    Json::Value body(Json::objectValue);
    body["args"] = args;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    req->setBody(Json::writeString(writer, body));
    auto attrs = req->attributes();
    attrs->insert(plinth::auth::ATTR_USER_ID, user_id);
    attrs->insert(plinth::auth::ATTR_USERNAME, username);
    attrs->insert(plinth::auth::ATTR_AUTH_TYPE, std::string{"session"});
    attrs->insert(plinth::auth::ATTR_SESSION_ID, std::string{});
    attrs->insert(plinth::auth::ATTR_PAT_ID, std::string{});
    attrs->insert(plinth::auth::ATTR_TOKEN_HASH, std::string{});

    auto promise = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
    auto future = promise->get_future();
    plinth::cap::test_seam::dispatch_post_cap(
        req,
        [promise](const drogon::HttpResponsePtr& response) {
          promise->set_value(response);
        },
        cfg.db, "shell." + std::string{function});
    return future.get();
  }

  auto row_count(std::string_view key, std::string id = {}) const -> int {
    PgConnection pg(cfg.db);
    auto result =
        pg.exec_params("SELECT count(*) FROM ext_shell.user_preferences "
                       "WHERE user_id = $1::uuid AND key = $2",
                       {id.empty() ? user_id : id, std::string{key}});
    return std::stoi(PQgetvalue(result.get(), 0, 0));
  }

  auto audit_count(std::string_view action, std::string_view key = {}) const
      -> int {
    PgConnection pg(cfg.db);
    auto result =
        pg.exec_params("SELECT count(*) FROM plinth.audit_log "
                       "WHERE action = $1 AND user_id = $2::uuid "
                       "AND ($3 = '' OR detail->>'key' = $3)",
                       {std::string{action}, user_id, std::string{key}});
    return std::stoi(PQgetvalue(result.get(), 0, 0));
  }

  auto wait_for_audit(std::string_view key,
                      std::chrono::milliseconds timeout) const -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (audit_count("shell.preferences.set", key) >= 1) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  auto wait_for_audit_action(std::string_view action,
                             std::chrono::milliseconds timeout) const -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (audit_count(action) >= 1) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  auto audit_detail(std::string_view action) const -> Json::Value {
    PgConnection pg(cfg.db);
    auto result = pg.exec_params("SELECT detail::text FROM plinth.audit_log "
                                 "WHERE action = $1 AND user_id = $2::uuid "
                                 "ORDER BY timestamp DESC LIMIT 1",
                                 {std::string{action}, user_id});
    REQUIRE(PQntuples(result.get()) == 1);
    Json::Value detail;
    Json::CharReaderBuilder reader;
    std::string errors;
    auto text = std::string{PQgetvalue(result.get(), 0, 0)};
    auto parser = std::unique_ptr<Json::CharReader>{reader.newCharReader()};
    REQUIRE(parser->parse(text.data(), text.data() + text.size(), &detail,
                          &errors));
    return detail;
  }

  auto delete_user(std::string_view id) const -> void {
    PgConnection pg(cfg.db);
    pg.exec_params("DELETE FROM plinth.users WHERE id = $1::uuid",
                   {std::string{id}});
  }

  const std::string& id() const { return user_id; }

 private:
  auto make_admin(std::string_view id) const -> void {
    PgConnection pg(cfg.db);
    pg.exec_params(
        "INSERT INTO plinth.group_members (group_id, user_id) "
        "SELECT id, $1::uuid FROM plinth.groups WHERE name = 'admin' "
        "ON CONFLICT DO NOTHING",
        {std::string{id}});
  }

  plinth::Config cfg;
  fs::path root;
  std::string username;
  std::string user_id;
  std::vector<std::string> successful_set_keys;
};

auto args_with(std::string_view key, const Json::Value& value) -> Json::Value {
  Json::Value args(Json::objectValue);
  args["key"] = std::string{key};
  args["value"] = value;
  return args;
}

auto get_args(std::string_view key) -> Json::Value {
  Json::Value args(Json::objectValue);
  args["key"] = std::string{key};
  return args;
}

auto require_value(ShellDispatchFixture& fixture, std::string_view key,
                   const Json::Value& expected) -> void {
  auto result = fixture.call("preferences.get", get_args(key));
  REQUIRE(result.has_value());
  REQUIRE(result->data["value"] == expected);
}

auto response_body(const drogon::HttpResponsePtr& response) -> Json::Value {
  Json::Value body;
  Json::CharReaderBuilder builder;
  std::string errors;
  auto text = std::string{response->body()};
  auto reader = std::unique_ptr<Json::CharReader>{builder.newCharReader()};
  REQUIRE(
      reader->parse(text.data(), text.data() + text.size(), &body, &errors));
  return body;
}

#define REQUIRE_PG()                                                           \
  do {                                                                         \
    if (!plinth::async_bridge_test::pg_available()) {                          \
      SKIP("PG not available");                                                \
    }                                                                          \
  } while (false)

} // namespace

TEST_CASE("P.01: preference insert and get round-trip",
          "[shell][preferences][integration][P.01]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p01");
  REQUIRE(fixture.call("preferences.set", args_with("theme.mode", "dark")));
  require_value(fixture, "theme.mode", "dark");
}

TEST_CASE("P.02: preference overwrite keeps one row",
          "[shell][preferences][integration][P.02]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p02");
  REQUIRE(fixture.call("preferences.set", args_with("theme.mode", "dark")));
  REQUIRE(fixture.call("preferences.set", args_with("theme.mode", "light")));
  require_value(fixture, "theme.mode", "light");
  REQUIRE(fixture.row_count("theme.mode") == 1);
  REQUIRE(fixture.wait_for_audit("theme.mode", 2s));
  REQUIRE(fixture.audit_count("shell.preferences.set", "theme.mode") == 1);
}

TEST_CASE("P.03: absent preference omits value",
          "[shell][preferences][integration][P.03]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p03");
  auto result = fixture.call("preferences.get", get_args("missing"));
  REQUIRE(result);
  REQUIRE(result->data.isObject());
  REQUIRE_FALSE(result->data.isMember("value"));
}

TEST_CASE("P.04: stored JSON null remains distinct from absence",
          "[shell][preferences][integration][P.04]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p04");
  REQUIRE(fixture.call("preferences.set",
                       args_with("scratch", Json::Value{Json::nullValue})));
  require_value(fixture, "scratch", Json::Value{Json::nullValue});
  REQUIRE(fixture.row_count("scratch") == 1);
}

TEST_CASE("P.05: omitted value deletes a preference",
          "[shell][preferences][integration][P.05]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p05");
  REQUIRE(fixture.call("preferences.set", args_with("scratch", "x")));
  auto deleted = fixture.call("preferences.set", get_args("scratch"));
  REQUIRE(deleted);
  REQUIRE(deleted->data["deleted"].asBool());
  REQUIRE(fixture.row_count("scratch") == 0);
  auto missing = fixture.call("preferences.get", get_args("scratch"));
  REQUIRE(missing);
  REQUIRE_FALSE(missing->data.isMember("value"));
}

TEST_CASE("P.06: arrays round-trip through preference dispatch",
          "[shell][preferences][integration][P.06]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p06");
  Json::Value value(Json::arrayValue);
  value.append("notes");
  value.append("files");
  value.append("kb");
  REQUIRE(
      fixture.call("preferences.set", args_with("topbar.tab_order", value)));
  require_value(fixture, "topbar.tab_order", value);
}

TEST_CASE("P.07: nested objects round-trip through preference dispatch",
          "[shell][preferences][integration][P.07]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p07");
  Json::Value value;
  value["a"][0] = 1.0;
  value["a"][1] = 2.0;
  value["a"][2]["b"] = "c";
  REQUIRE(fixture.call("preferences.set", args_with("complex", value)));
  require_value(fixture, "complex", value);
}

TEST_CASE("P.08: empty preference key returns HTTP 400",
          "[shell][preferences][integration][P.08]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p08");
  auto response = fixture.http_call("preferences.set", args_with("", "x"));
  REQUIRE(response->statusCode() == drogon::k400BadRequest);
  REQUIRE(response_body(response)["error"]["code"] == "invalid_argument");
}

TEST_CASE("P.09: overlong preference key returns HTTP 400",
          "[shell][preferences][integration][P.09]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p09");
  auto response = fixture.http_call("preferences.set",
                                    args_with(std::string(256, 'k'), "x"));
  REQUIRE(response->statusCode() == drogon::k400BadRequest);
  REQUIRE(response_body(response)["error"]["code"] == "invalid_argument");
}

TEST_CASE("P.10: oversized preference value returns HTTP 413",
          "[shell][preferences][integration][P.10]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p10");
  auto response = fixture.http_call(
      "preferences.set", args_with("large", std::string(65536, 'x')));
  REQUIRE(response->statusCode() == drogon::k413RequestEntityTooLarge);
  REQUIRE(response_body(response)["error"]["code"] == "payload_too_large");
}

TEST_CASE("P.11: preferences are isolated by caller identity",
          "[shell][preferences][integration][P.11]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p11_a");
  REQUIRE(fixture.call("preferences.set", args_with("k", "A-val")));
  auto second_id = fixture.add_user("p11_b");
  auto second = fixture.context(second_id);
  auto result = fixture.call("preferences.get", get_args("k"), &second);
  REQUIRE(result);
  REQUIRE_FALSE(result->data.isMember("value"));
}

TEST_CASE("P.12: preference read requires its RBAC rule",
          "[shell][preferences][integration][P.12]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p12");
  auto denied = fixture.context({}, {});
  auto result = fixture.call("preferences.get", get_args("k"), &denied);
  REQUIRE_FALSE(result);
  REQUIRE(result.error() ==
          plinth::capabilities::CapabilityError::PERMISSION_DENIED);
}

TEST_CASE("P.13: get_all is sorted and empty for a new user",
          "[shell][preferences][integration][P.13]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p13");
  auto empty = fixture.call("preferences.get_all", Json::Value{});
  REQUIRE(empty);
  REQUIRE(empty->data["entries"].empty());
  REQUIRE(fixture.call("preferences.set", args_with("z", 1)));
  REQUIRE(fixture.call("preferences.set", args_with("a", 2)));
  auto result = fixture.call("preferences.get_all", Json::Value{});
  REQUIRE(result);
  REQUIRE(result->data["entries"].size() == 2);
  REQUIRE(result->data["entries"][0]["key"] == "a");
  REQUIRE(result->data["entries"][1]["key"] == "z");
}

TEST_CASE("P.14: deleting a user cascades preference rows",
          "[shell][preferences][integration][P.14]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("p14");
  REQUIRE(fixture.call("preferences.set", args_with("k", "value")));
  fixture.delete_user(fixture.id());
  REQUIRE(fixture.row_count("k") == 0);
}

TEST_CASE("T.01: light theme round-trips",
          "[shell][preferences][integration][T.01]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t01");
  REQUIRE(fixture.call("preferences.set", args_with("shell.theme", "light")));
  require_value(fixture, "shell.theme", "light");
}

TEST_CASE("T.02: dark theme round-trips",
          "[shell][preferences][integration][T.02]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t02");
  REQUIRE(fixture.call("preferences.set", args_with("shell.theme", "dark")));
  require_value(fixture, "shell.theme", "dark");
}

TEST_CASE("T.03: system theme round-trips",
          "[shell][preferences][integration][T.03]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t03");
  REQUIRE(fixture.call("preferences.set", args_with("shell.theme", "system")));
  require_value(fixture, "shell.theme", "system");
}

TEST_CASE("T.04: invalid theme is rejected",
          "[shell][preferences][integration][T.04]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t04");
  std::string code;
  auto result = fixture.call(
      "preferences.set", args_with("shell.theme", "fuchsia"), nullptr, &code);
  REQUIRE_FALSE(result);
  REQUIRE(code == "invalid_argument");
}

TEST_CASE("T.05: non-string theme is rejected",
          "[shell][preferences][integration][T.05]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t05");
  std::string code;
  auto result = fixture.call("preferences.set", args_with("shell.theme", 42),
                             nullptr, &code);
  REQUIRE_FALSE(result);
  REQUIRE(code == "invalid_argument");
}

TEST_CASE("T.06: absent theme omits value",
          "[shell][preferences][integration][T.06]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t06");
  auto result = fixture.call("preferences.get", get_args("shell.theme"));
  REQUIRE(result);
  REQUIRE_FALSE(result->data.isMember("value"));
}

TEST_CASE("T.07: deleting theme restores absence",
          "[shell][preferences][integration][T.07]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t07");
  REQUIRE(fixture.call("preferences.set", args_with("shell.theme", "light")));
  REQUIRE(fixture.call("preferences.set", get_args("shell.theme")));
  auto result = fixture.call("preferences.get", get_args("shell.theme"));
  REQUIRE(result);
  REQUIRE_FALSE(result->data.isMember("value"));
}

TEST_CASE("T.08: theme mutation emits privacy-preserving audit",
          "[shell][preferences][integration][T.08]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("t08");
  REQUIRE(fixture.call("preferences.set", args_with("shell.theme", "light")));
  REQUIRE(fixture.wait_for_audit("shell.theme", 2s));
  auto detail = fixture.audit_detail("shell.preferences.set");
  REQUIRE(detail["key"] == "shell.theme");
  REQUIRE(detail["value_class"] == "string");
  REQUIRE(detail["value_size"] == 7);
  REQUIRE_FALSE(detail.isMember("value"));
}

TEST_CASE("S.01: scale 100 round-trips",
          "[shell][preferences][integration][S.01]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s01");
  REQUIRE(fixture.call("preferences.set", args_with("shell.scale_pct", 100)));
  require_value(fixture, "shell.scale_pct", 100.0);
}

TEST_CASE("S.02: minimum scale round-trips",
          "[shell][preferences][integration][S.02]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s02");
  REQUIRE(fixture.call("preferences.set", args_with("shell.scale_pct", 80)));
  require_value(fixture, "shell.scale_pct", 80.0);
}

TEST_CASE("S.03: maximum scale round-trips",
          "[shell][preferences][integration][S.03]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s03");
  REQUIRE(fixture.call("preferences.set", args_with("shell.scale_pct", 175)));
  require_value(fixture, "shell.scale_pct", 175.0);
}

TEST_CASE("S.04: below-minimum scale is rejected",
          "[shell][preferences][integration][S.04]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s04");
  std::string code;
  auto result = fixture.call("preferences.set",
                             args_with("shell.scale_pct", 79), nullptr, &code);
  REQUIRE_FALSE(result);
  REQUIRE(code == "invalid_argument");
}

TEST_CASE("S.05: above-maximum scale is rejected",
          "[shell][preferences][integration][S.05]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s05");
  std::string code;
  auto result = fixture.call("preferences.set",
                             args_with("shell.scale_pct", 176), nullptr, &code);
  REQUIRE_FALSE(result);
  REQUIRE(code == "invalid_argument");
}

TEST_CASE("S.06: fractional scale is rejected",
          "[shell][preferences][integration][S.06]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s06");
  std::string code;
  auto result = fixture.call(
      "preferences.set", args_with("shell.scale_pct", 100.5), nullptr, &code);
  REQUIRE_FALSE(result);
  REQUIRE(code == "invalid_argument");
}

TEST_CASE("S.07: string scale is rejected",
          "[shell][preferences][integration][S.07]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s07");
  std::string code;
  auto result = fixture.call(
      "preferences.set", args_with("shell.scale_pct", "100"), nullptr, &code);
  REQUIRE_FALSE(result);
  REQUIRE(code == "invalid_argument");
}

TEST_CASE("S.08: absent scale omits value",
          "[shell][preferences][integration][S.08]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("s08");
  auto result = fixture.call("preferences.get", get_args("shell.scale_pct"));
  REQUIRE(result);
  REQUIRE_FALSE(result->data.isMember("value"));
}

TEST_CASE("A.01: boundary audit dispatch writes sanitized identity",
          "[shell][preferences][integration][A.01]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("a01");
  Json::Value detail;
  detail["panel_id"] = "demo";
  detail["error_message"] = "boom";
  detail["error_stack"] = "stack";
  detail["component_path"] = "App/Demo";
  auto result = fixture.call("audit.emit", detail);
  REQUIRE(result);
  REQUIRE(
      fixture.wait_for_audit_action("ext.shell.frontend.boundary.caught", 2s));
  REQUIRE(fixture.audit_count("ext.shell.frontend.boundary.caught") == 1);
  auto stored = fixture.audit_detail("ext.shell.frontend.boundary.caught");
  REQUIRE(stored["panel_id"] == "demo");
  REQUIRE(stored["error_message"] == "boom");
  REQUIRE(stored["error_stack"] == "stack");
  REQUIRE(stored["component_path"] == "App/Demo");
  REQUIRE_FALSE(stored.isMember("user_id"));
}

TEST_CASE("A.02: boundary audit action override is ignored",
          "[shell][preferences][integration][A.02]") {
  REQUIRE_PG();
  ShellDispatchFixture fixture("a02");
  Json::Value detail;
  detail["panel_id"] = "demo";
  detail["error_message"] = "boom";
  detail["action"] = "user.login.success";
  auto result = fixture.call("audit.emit", detail);
  REQUIRE(result);
  REQUIRE(
      fixture.wait_for_audit_action("ext.shell.frontend.boundary.caught", 2s));
  REQUIRE(fixture.audit_count("ext.shell.frontend.boundary.caught") == 1);
  REQUIRE(fixture.audit_count("user.login.success") == 0);
  REQUIRE_FALSE(fixture.audit_detail("ext.shell.frontend.boundary.caught")
                    .isMember("action"));
}
