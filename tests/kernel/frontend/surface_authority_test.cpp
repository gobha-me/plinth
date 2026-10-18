#include "../ws/ws_test_fixture.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/frontend/surface_authority.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace authority = plinth::frontend::authority;
using SurfaceJson = nlohmann::json;
using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
constexpr auto USER = "10000000-0000-4000-8000-000000000046";
constexpr auto SESSION = "20000000-0000-4000-8000-000000000046";
constexpr auto PAT = "30000000-0000-4000-8000-000000000046";

auto config() -> plinth::Config::Database {
  plinth::Config::Database db;
  REQUIRE(std::getenv("PLINTH_PG_HOST") != nullptr);
  db.host = std::getenv("PLINTH_PG_HOST");
  if (const auto* value = std::getenv("PLINTH_PG_PORT"))
    db.port = static_cast<std::uint16_t>(std::stoi(value));
  if (const auto* value = std::getenv("PLINTH_PG_USER")) db.user = value;
  if (const auto* value = std::getenv("PLINTH_PG_PASSWORD"))
    db.password = value;
  if (const auto* value = std::getenv("PLINTH_PG_DATABASE"))
    db.database = value;
  return db;
}

struct Fixture {
  plinth::Config::Database db{config()};
  Connection connection{nullptr, PQfinish};
  plinth::auth::AuthContext auth{.user_id = USER,
                                 .username = "untrusted-old-username",
                                 .auth_type = "session",
                                 .session_id = SESSION,
                                 .pat_id = {},
                                 .token_hash = std::string(64, 'a'),
                                 .credential_source =
                                     plinth::auth::CredentialSource::COOKIE};
  Fixture() {
    plinth::ws_test::reset_schema(db);
    connection.reset(
        plinth::db::connect(plinth::db::connection_info(db).c_str()));
    REQUIRE(connection);
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
    run("INSERT INTO plinth.users(id,username,password_hash) "
        "VALUES($1::uuid,'surface-user','unused')",
        {USER});
    run("INSERT INTO plinth.sessions(id,user_id,token_hash) "
        "VALUES($1::uuid,$2::uuid,$3)",
        {SESSION, USER, auth.token_hash});
    run("INSERT INTO plinth.pats(id,user_id,name,token_hash,token_prefix) "
        "VALUES($1::uuid,$2::uuid,'fixture',$3,'plinth_fake')",
        {PAT, USER, std::string(64, 'b')});
  }
  ~Fixture() { plinth::ws_test::settle_fixture_work_or_exit(); }
  auto run(const std::string& sql, const std::vector<std::string>& values = {})
      -> void {
    std::vector<const char*> pointers;
    pointers.reserve(values.size());
    for (const auto& value : values)
      pointers.push_back(value.c_str());
    Result result{plinth::db::exec_params(connection.get(), sql.c_str(),
                                          static_cast<int>(pointers.size()),
                                          nullptr, pointers.data(), nullptr,
                                          nullptr, 0),
                  PQclear};
    REQUIRE(result);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto grant(std::string rule, std::string owner, bool granted = true) -> void {
    run("INSERT INTO "
        "plinth.rbac_rules(rule,namespace,description,extension_name) "
        "VALUES($1,$2,'fixture',$2)",
        {rule, owner});
    if (granted)
      run("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
          "FROM plinth.groups g,plinth.rbac_rules r WHERE g.name='everyone' "
          "AND r.rule=$1",
          {rule});
  }
  auto seed(std::string name = "notes", std::string rank = "1",
            std::string panel = "preview") -> void {
    grant(name + ".panel", name);
    grant(name + ".call", name);
    run("INSERT INTO "
        "plinth.packages(name,version,state,provenance,manifest_json,entry_"
        "point,manifest_checksum,application_ready) "
        "VALUES($1,'1.0.0','ACTIVE','user','{}','server/"
        "main.js','fixture',true)",
        {name});
    const auto signature = name + ":1:preview";
    run("INSERT INTO "
        "plinth.capabilities(namespace,version,function,signature,provider_"
        "type,extension_name,description,rbac_rule) "
        "VALUES($1,1,'preview',$2,'extension',$1,'fixture',$3)",
        {name, signature, name + ".call"});
    const SurfaceJson declaration{
        {"id", panel},
        {"type", "float"},
        {"client_path", "preview.js"},
        {"rbac_rule", name + ".panel"},
        {"capability", signature},
        {"content_types", SurfaceJson::array({"text/plain"})},
        {"context_schema",
         {{"version", 1},
          {"fields",
           {{"record_id",
             {{"required", true},
              {"max_bytes", 32},
              {"meaning", "stable ID"}}}}},
          {"persist_keys", SurfaceJson::array({"record_id"})}}}};
    // Direct SQL fixtures must still satisfy the shipping typed parser. An
    // application identifier may contain '-', but the derived RBAC names may
    // not; invalid setup is not evidence of a catalog/pagination regression.
    const auto validated = plinth::packages::PanelsManifest::parse(
        SurfaceJson{{"format_version", 2},
                    {"panels", SurfaceJson::array({declaration})}}
            .dump(),
        "surface authority fixture");
    REQUIRE(validated.value);
    run("INSERT INTO "
        "plinth.panels(package_id,panel_id,panel_type,declaration,validated_"
        "format_version,logical_install_order) SELECT "
        "id,$2,'float',$3::jsonb,2,$4::numeric FROM plinth.packages WHERE "
        "name=$1",
        {name, panel, declaration.dump(), rank});
  }
  auto lookup() -> std::expected<authority::LookupResult, authority::Error> {
    return authority::lookup_surface(*connection, auth, "notes", "preview",
                                     plinth::frontend::SurfaceKind::FLOAT);
  }
  auto seed_surface(std::string name, plinth::frontend::SurfaceKind kind,
                    std::string panel = "preview", bool granted = true,
                    bool navigation = true) -> void {
    REQUIRE((kind == plinth::frontend::SurfaceKind::PRIMARY ||
             kind == plinth::frontend::SurfaceKind::TRAY));
    const std::string type =
        kind == plinth::frontend::SurfaceKind::PRIMARY ? "primary" : "tray";
    grant(name + ".panel", name, granted);
    run("INSERT INTO "
        "plinth.packages(name,version,state,provenance,manifest_json,"
        "entry_point,manifest_checksum,application_ready) "
        "VALUES($1,'1.0.0','ACTIVE','user','{}','server/"
        "main.js','fixture',true)",
        {name});
    SurfaceJson declaration{{"id", panel},
                            {"type", type},
                            {"client_path", "preview.js"},
                            {"rbac_rule", name + ".panel"}};
    if (kind == plinth::frontend::SurfaceKind::PRIMARY) {
      declaration["accepts_navigation_intent"] = navigation;
      declaration["context_schema"] = {{"version", 1},
                                       {"fields", SurfaceJson::object()},
                                       {"persist_keys", SurfaceJson::array()}};
    } else {
      declaration["tray_states"] = {{"idle", "circle"}};
      declaration["tray_initial_state"] = "idle";
      declaration["preferred_width"] = 320;
    }
    const auto validated = plinth::packages::PanelsManifest::parse(
        SurfaceJson{{"format_version", 2},
                    {"panels", SurfaceJson::array({declaration})}}
            .dump(),
        "nonfloat surface authority fixture");
    REQUIRE(validated.value);
    run("INSERT INTO plinth.panels(package_id,panel_id,panel_type,declaration,"
        "validated_format_version,logical_install_order) "
        "SELECT id,$2,$3,$4::jsonb,2,NULL FROM plinth.packages WHERE name=$1",
        {name, panel, type, declaration.dump()});
  }
};
} // namespace

TEST_CASE("surface authority: exact fresh credential and current rule gates",
          "[frontend][surface-authority][integration]") {
  Fixture fixture;
  auto user = authority::authenticate(*fixture.connection, fixture.auth);
  REQUIRE(user);
  REQUIRE(user->username == "surface-user");
  REQUIRE(user->session_id == SESSION);
  // Fresh bootstrap intentionally grants the application-change subscription
  // to everyone. Authentication must preserve that real baseline, without
  // inventing either extension access or kernel.admin.
  REQUIRE(user->effective_rules ==
          std::vector<std::string>{
              "kernel.realtime.subscribe.applications.changed"});
  SECTION(
      "session identity hash expiry revocation and user disable are current") {
    auto mismatch = fixture.auth;
    mismatch.token_hash = std::string(64, 'c');
    REQUIRE(authority::authenticate(*fixture.connection, mismatch).error() ==
            authority::Error::UNAUTHENTICATED);
    mismatch = fixture.auth;
    mismatch.user_id = "10000000-0000-4000-8000-000000000047";
    REQUIRE(authority::authenticate(*fixture.connection, mismatch).error() ==
            authority::Error::UNAUTHENTICATED);
    fixture.run("UPDATE plinth.sessions SET "
                "expires_at=clock_timestamp()-interval '1 second'");
    REQUIRE(
        authority::authenticate(*fixture.connection, fixture.auth).error() ==
        authority::Error::UNAUTHENTICATED);
    fixture.run(
        "UPDATE plinth.sessions SET expires_at=clock_timestamp()+interval '1 "
        "hour',revoked_at=clock_timestamp()");
    REQUIRE(
        authority::authenticate(*fixture.connection, fixture.auth).error() ==
        authority::Error::UNAUTHENTICATED);
    fixture.run("UPDATE plinth.sessions SET revoked_at=NULL");
    fixture.run("UPDATE plinth.users SET disabled_at=clock_timestamp() WHERE "
                "id=$1::uuid",
                {USER});
    REQUIRE(
        authority::authenticate(*fixture.connection, fixture.auth).error() ==
        authority::Error::UNAUTHENTICATED);
  }
  SECTION("PAT has its own credential id hash nullable expiry and original "
          "identity") {
    auto pat = fixture.auth;
    pat.auth_type = "pat";
    pat.session_id.clear();
    pat.pat_id = PAT;
    pat.token_hash = std::string(64, 'b');
    pat.credential_source = plinth::auth::CredentialSource::BEARER;
    auto authenticated = authority::authenticate(*fixture.connection, pat);
    REQUIRE(authenticated);
    REQUIRE(authenticated->session_id.empty());
    REQUIRE(authenticated->user_id == USER);
    REQUIRE(authenticated->auth_type == "pat");
    fixture.run("UPDATE plinth.pats SET expires_at=clock_timestamp()-interval "
                "'1 second'");
    REQUIRE(authority::authenticate(*fixture.connection, pat).error() ==
            authority::Error::UNAUTHENTICATED);
    fixture.run(
        "UPDATE plinth.pats SET expires_at=NULL,revoked_at=clock_timestamp()");
    REQUIRE(authority::authenticate(*fixture.connection, pat).error() ==
            authority::Error::UNAUTHENTICATED);
  }
  SECTION("effective rules and reviewer eligibility are fresh, without "
          "impersonation") {
    REQUIRE(
        authority::require_rule(*fixture.connection, fixture.auth, "notes.read")
            .error() == authority::Error::DENIED);
    REQUIRE_FALSE(*authority::reviewer_is_admin(*fixture.connection, USER));
    fixture.grant("notes.read", "notes");
    REQUIRE(authority::require_rule(*fixture.connection, fixture.auth,
                                    "notes.read"));
    fixture.run("UPDATE plinth.rbac_rules SET orphaned_at=clock_timestamp() "
                "WHERE rule='notes.read'");
    REQUIRE(
        authority::require_rule(*fixture.connection, fixture.auth, "notes.read")
            .error() == authority::Error::DENIED);
    fixture.run("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT "
                "g.id,r.id FROM plinth.groups g,plinth.rbac_rules r WHERE "
                "g.name='everyone' AND r.rule='kernel.admin'");
    REQUIRE(*authority::reviewer_is_admin(*fixture.connection, USER));
    REQUIRE(authority::require_rule(*fixture.connection, fixture.auth,
                                    "notes.read"));
    fixture.run("UPDATE plinth.users SET disabled_at=clock_timestamp() WHERE "
                "id=$1::uuid",
                {USER});
    REQUIRE_FALSE(*authority::reviewer_is_admin(*fixture.connection, USER));
  }
  SECTION("fixed preference gate requires current registered shell provider "
          "and rule") {
    REQUIRE(authority::require_shell_preference_rule(*fixture.connection,
                                                     fixture.auth, false)
                .error() == authority::Error::DENIED);
    fixture.grant("shell.preferences.read", "shell");
    fixture.run("INSERT INTO "
                "plinth.capabilities(namespace,version,function,signature,"
                "provider_type,extension_name,description,rbac_rule) "
                "VALUES('shell',1,'preferences.get','shell:1:preferences.get','"
                "extension','shell','fixture','shell.preferences.read')");
    REQUIRE(authority::require_shell_preference_rule(*fixture.connection,
                                                     fixture.auth, false));
    fixture.run(
        "UPDATE plinth.capabilities SET enabled=false WHERE namespace='shell'");
    REQUIRE(authority::require_shell_preference_rule(*fixture.connection,
                                                     fixture.auth, false)
                .error() == authority::Error::DENIED);
  }
  SECTION("fixed write gate is distinct and admin cannot invent a registered "
          "binding") {
    fixture.grant("shell.preferences.write", "shell");
    fixture.run(
        "INSERT INTO plinth.capabilities(namespace,version,function,signature,"
        "provider_type,extension_name,description,rbac_rule) VALUES('shell',1,"
        "'preferences.set','shell:1:preferences.set','extension','shell',"
        "'fixture','shell.preferences.write')");
    auto write = [&] {
      return authority::require_shell_preference_rule(*fixture.connection,
                                                      fixture.auth, true);
    };
    REQUIRE(write());
    auto read = authority::require_shell_preference_rule(*fixture.connection,
                                                         fixture.auth, false);
    REQUIRE_FALSE(read);
    REQUIRE(read.error() == authority::Error::DENIED);
    fixture.run("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT id FROM "
                "plinth.rbac_rules WHERE rule='shell.preferences.write')");
    auto denied = write();
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error() == authority::Error::DENIED);
    fixture.run(
        "INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
        "FROM plinth.groups g,plinth.rbac_rules r WHERE g.name='everyone' "
        "AND r.rule='kernel.admin'");
    REQUIRE(write()); // Universal permission is not universal registration.
    SECTION("foreign registered rule") {
      fixture.run("UPDATE plinth.rbac_rules SET extension_name='foreign' "
                  "WHERE rule='shell.preferences.write'");
    }
    SECTION("orphaned registered rule") {
      fixture.run("UPDATE plinth.rbac_rules SET orphaned_at=clock_timestamp() "
                  "WHERE rule='shell.preferences.write'");
    }
    SECTION("unsupported stored user override") {
      fixture.run(
          "INSERT INTO "
          "plinth.capabilities(namespace,version,function,signature,"
          "provider_type,extension_name,scope,description,rbac_rule,enabled) "
          "VALUES('shell',1,'preferences.set','shell:1:preferences.set',"
          "'extension','foreign','user','fixture','shell.preferences.write',"
          "false)");
    }
    SECTION("ambiguous instance signature") {
      fixture.run(
          "INSERT INTO "
          "plinth.capabilities(namespace,version,function,signature,"
          "provider_type,extension_name,description,rbac_rule) "
          "VALUES('shell',1,"
          "'other','shell:1:preferences.set','extension','shell','fixture',"
          "'shell.preferences.write')");
    }
    SECTION("disabled provider") {
      fixture.run("UPDATE plinth.capabilities SET enabled=false WHERE "
                  "namespace='shell'");
    }
    SECTION("missing provider registration") {
      fixture.run("DELETE FROM plinth.capabilities WHERE namespace='shell'");
    }
    SECTION("foreign provider") {
      fixture.run("UPDATE plinth.capabilities SET extension_name='foreign' "
                  "WHERE namespace='shell'");
    }
    auto refused = write();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == authority::Error::DENIED);
  }
  SECTION("database failure remains unknown not missing or credential denial") {
    fixture.run("BEGIN");
    Result failed{PQexec(fixture.connection.get(), "SELECT 1/0"), PQclear};
    REQUIRE(PQresultStatus(failed.get()) == PGRES_FATAL_ERROR);
    auto unknown = authority::authenticate(*fixture.connection, fixture.auth);
    REQUIRE_FALSE(unknown);
    REQUIRE(unknown.error() == authority::Error::UNAVAILABLE);
    fixture.run("ROLLBACK");
  }
}

TEST_CASE("surface authority: both gates provider ownership and bounded "
          "ordinal prefix",
          "[frontend][surface-authority][integration]") {
  Fixture fixture;
  fixture.seed();
  auto initial = fixture.lookup();
  REQUIRE(initial);
  REQUIRE(initial->target);
  REQUIRE(initial->target->declaration.capability == "notes:1:preview");
  REQUIRE(initial->target->capability_rule == "notes.call");
  REQUIRE(initial->caller.user_id == USER);
  SECTION("primary and tray prefixes isolate kinds and filter before bytewise "
          "limit") {
    using enum plinth::frontend::SurfaceKind;
    for (const auto kind : {PRIMARY, TRAY}) {
      const std::string stem = kind == PRIMARY ? "primary" : "tray";
      fixture.seed_surface(stem + "a", kind, "zeta", false);
      fixture.seed_surface(stem + "b", kind, "zeta");
      fixture.seed_surface(stem + "c", kind);
      fixture.run(
          "INSERT INTO "
          "plinth.panels(package_id,panel_id,panel_type,declaration,"
          "validated_format_version) SELECT pn.package_id,'pre-view',"
          "pn.panel_type,jsonb_set(pn.declaration,'{id}','\"pre-view\"'),2 "
          "FROM plinth.panels pn JOIN plinth.packages p ON p.id=pn.package_id "
          "WHERE p.name=$1 AND pn.panel_id='zeta'",
          {stem + "b"});
      const std::vector<std::pair<std::string, std::string>> expected{
          {stem + "b", "pre-view"},
          {stem + "b", "zeta"},
          {stem + "c", "preview"}};
      std::optional<authority::Position> after;
      for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto page = authority::catalog_surface_prefix(
            *fixture.connection, fixture.auth, kind, 1, after);
        REQUIRE(page);
        REQUIRE(page->targets.size() == 1);
        REQUIRE(page->targets[0].kind == kind);
        REQUIRE(page->targets[0].application_id == expected[index].first);
        REQUIRE(page->targets[0].declaration.id == expected[index].second);
        REQUIRE_FALSE(page->targets[0].logical_install_order);
        REQUIRE_FALSE(page->targets[0].declaration.capability);
        const authority::Position position{std::nullopt, expected[index].first,
                                           expected[index].second};
        if (index + 1 < expected.size()) {
          REQUIRE(page->next_position == position);
        } else {
          REQUIRE_FALSE(page->next_position);
        }
        after = position;
      }
      const auto exhausted = authority::catalog_surface_prefix(
          *fixture.connection, fixture.auth, kind, 1, after);
      REQUIRE(exhausted);
      REQUIRE(exhausted->targets.empty());
      REQUIRE_FALSE(exhausted->next_position);
    }
    fixture.seed_surface("initialonly", PRIMARY, "preview", true, false);
    const auto receivers = authority::catalog_surface_prefix(
        *fixture.connection, fixture.auth, PRIMARY, 64);
    REQUIRE(receivers);
    REQUIRE(receivers->targets.size() == 3);
    REQUIRE(authority::catalog_surface_prefix(*fixture.connection, fixture.auth,
                                              FLOAT, 1)
                .error() == authority::Error::INVALID);
    for (const auto limit : {std::size_t{0}, std::size_t{65}})
      REQUIRE(authority::catalog_surface_prefix(*fixture.connection,
                                                fixture.auth, TRAY, limit)
                  .error() == authority::Error::INVALID);
    for (const auto& position :
         {authority::Position{"1", "trayb", "preview"},
          authority::Position{std::nullopt, "../trayb", "preview"},
          authority::Position{std::nullopt, "trayb", "../preview"}})
      REQUIRE(authority::catalog_surface_prefix(*fixture.connection,
                                                fixture.auth, TRAY, 1, position)
                  .error() == authority::Error::INVALID);
    REQUIRE(authority::catalog_float_prefix(
                *fixture.connection, fixture.auth, "text/plain", 1,
                authority::Position{std::nullopt, "notes", "preview"})
                .error() == authority::Error::INVALID);
  }
  SECTION(
      "nonfloat maximum prefix has one lookahead without an invented ordinal") {
    using enum plinth::frontend::SurfaceKind;
    for (const auto kind : {PRIMARY, TRAY}) {
      const std::string stem = kind == PRIMARY ? "primary" : "tray";
      std::vector<std::string> expected;
      for (unsigned int index = 0; index < 65; ++index) {
        const auto name = stem + std::to_string(index);
        fixture.seed_surface(name, kind);
        expected.push_back(name);
      }
      std::ranges::sort(expected);
      const auto first = authority::catalog_surface_prefix(
          *fixture.connection, fixture.auth, kind, 64);
      REQUIRE(first);
      REQUIRE(first->targets.size() == 64);
      for (std::size_t index = 0; index < 64; ++index)
        REQUIRE(first->targets[index].application_id == expected[index]);
      REQUIRE(first->next_position);
      REQUIRE_FALSE(first->next_position->logical_install_order);
      REQUIRE(first->next_position->application_id == expected[63]);
      const auto last = authority::catalog_surface_prefix(
          *fixture.connection, fixture.auth, kind, 64, first->next_position);
      REQUIRE(last);
      REQUIRE(last->targets.size() == 1);
      REQUIRE(last->targets[0].application_id == expected[64]);
      REQUIRE_FALSE(last->next_position);
    }
  }
  SECTION("nonfloat authority is fresh and metadata cannot bypass owned rule "
          "or provenance") {
    using enum plinth::frontend::SurfaceKind;
    fixture.seed_surface("receiver", PRIMARY);
    fixture.seed_surface("trayapp", TRAY);
    auto catalog = [&](auto kind) {
      return authority::catalog_surface_prefix(*fixture.connection,
                                               fixture.auth, kind, 64);
    };
    for (const auto kind : {PRIMARY, TRAY}) {
      const auto page = catalog(kind);
      REQUIRE(page);
      REQUIRE(page->targets.size() == 1);
    }
    SECTION("revoked presentation grant never leaks target metadata") {
      fixture.run("DELETE FROM plinth.group_rules WHERE rule_id IN "
                  "(SELECT id FROM plinth.rbac_rules WHERE extension_name IN "
                  "('receiver','trayapp'))");
    }
    SECTION("foreign ownership remains denied even to administrator") {
      fixture.run(
          "INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
          "FROM plinth.groups g,plinth.rbac_rules r WHERE g.name='everyone' "
          "AND r.rule='kernel.admin'");
      fixture.run("UPDATE plinth.rbac_rules SET extension_name='foreign' "
                  "WHERE extension_name IN ('receiver','trayapp')");
    }
    SECTION("orphaned rules remain denied") {
      fixture.run("UPDATE plinth.rbac_rules SET orphaned_at=clock_timestamp() "
                  "WHERE extension_name IN ('receiver','trayapp')");
    }
    SECTION("unready packages remain absent") {
      fixture.run("UPDATE plinth.packages SET application_ready=false WHERE "
                  "name IN ('receiver','trayapp')");
    }
    SECTION("legacy marker cannot activate typed authority") {
      fixture.run("UPDATE plinth.panels SET validated_format_version=0 WHERE "
                  "panel_type IN ('primary','tray')");
    }
    for (const auto kind : {PRIMARY, TRAY}) {
      const auto page = catalog(kind);
      REQUIRE(page);
      REQUIRE(page->targets.empty());
    }
  }
  SECTION("nonfloat PAT auth failure and database unknown are never "
          "authoritative absence") {
    using enum plinth::frontend::SurfaceKind;
    fixture.seed_surface("receiver", PRIMARY);
    auto pat = fixture.auth;
    pat.auth_type = "pat";
    pat.session_id.clear();
    pat.pat_id = PAT;
    pat.token_hash = std::string(64, 'b');
    pat.credential_source = plinth::auth::CredentialSource::BEARER;
    const auto page =
        authority::catalog_surface_prefix(*fixture.connection, pat, PRIMARY, 1);
    REQUIRE(page);
    REQUIRE(page->targets.size() == 1);
    fixture.run("UPDATE plinth.pats SET revoked_at=clock_timestamp()");
    REQUIRE(
        authority::catalog_surface_prefix(*fixture.connection, pat, PRIMARY, 1)
            .error() == authority::Error::UNAUTHENTICATED);
    fixture.run("UPDATE plinth.sessions SET revoked_at=clock_timestamp()");
    REQUIRE(authority::catalog_surface_prefix(*fixture.connection, fixture.auth,
                                              PRIMARY, 1)
                .error() == authority::Error::UNAUTHENTICATED);
    fixture.run("UPDATE plinth.sessions SET revoked_at=NULL");
    fixture.run("BEGIN");
    const Result failed{PQexec(fixture.connection.get(), "SELECT 1/0"),
                        PQclear};
    REQUIRE(PQresultStatus(failed.get()) == PGRES_FATAL_ERROR);
    REQUIRE(authority::catalog_surface_prefix(*fixture.connection, fixture.auth,
                                              PRIMARY, 1)
                .error() == authority::Error::UNAVAILABLE);
    fixture.run("ROLLBACK");
  }
  SECTION("panel and capability rules are independent and admin cannot bypass "
          "ownership") {
    fixture.run("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT id FROM "
                "plinth.rbac_rules WHERE rule='notes.call')");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT "
                "g.id,r.id FROM plinth.groups g,plinth.rbac_rules r WHERE "
                "g.name='everyone' AND r.rule='notes.call'");
    fixture.run("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT id FROM "
                "plinth.rbac_rules WHERE rule='notes.panel')");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT "
                "g.id,r.id FROM plinth.groups g,plinth.rbac_rules r WHERE "
                "g.name='everyone' AND r.rule='kernel.admin'");
    REQUIRE(fixture.lookup()->target);
    fixture.run("UPDATE plinth.rbac_rules SET extension_name='foreign' WHERE "
                "rule='notes.panel'");
    REQUIRE_FALSE(fixture.lookup()->target);
  }
  SECTION("disabled foreign provider and unsupported user override never fall "
          "back") {
    fixture.run(
        "UPDATE plinth.capabilities SET enabled=false WHERE namespace='notes'");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run(
        "UPDATE plinth.capabilities SET enabled=true,extension_name='foreign' "
        "WHERE namespace='notes'");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run("UPDATE plinth.capabilities SET extension_name='notes' WHERE "
                "namespace='notes'");
    fixture.run(
        "INSERT INTO "
        "plinth.capabilities(namespace,version,function,signature,provider_"
        "type,extension_name,scope,description,rbac_rule,enabled) "
        "VALUES('notes',1,'preview','notes:1:preview','extension','foreign','"
        "user','fixture','notes.call',false)");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run("DELETE FROM plinth.capabilities WHERE scope='user'");
    REQUIRE(fixture.lookup()->target);
  }
  SECTION("readiness provenance state and malformed stored declarations fail "
          "closed") {
    fixture.run("UPDATE plinth.panels SET "
                "validated_format_version=0,logical_install_order=NULL");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run("UPDATE plinth.panels SET "
                "validated_format_version=2,logical_install_order=1");
    fixture.run("UPDATE plinth.packages SET application_ready=false WHERE "
                "name='notes'");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run(
        "UPDATE plinth.packages SET state='DISABLED' WHERE name='notes'");
    REQUIRE_FALSE(fixture.lookup()->target);
    fixture.run("UPDATE plinth.packages SET "
                "state='ACTIVE',application_ready=true WHERE name='notes'");
    fixture.run(
        "UPDATE plinth.panels SET declaration=declaration-'context_schema'");
    auto malformed = fixture.lookup();
    REQUIRE_FALSE(malformed);
    REQUIRE(malformed.error() == authority::Error::UNAVAILABLE);
  }
  SECTION("catalog pagination preserves exact uint64 order and never skips "
          "earlier unauthorized rows") {
    fixture.run(
        "UPDATE plinth.panels SET logical_install_order=9007199254740993");
    fixture.seed("earliest", "9007199254740992");
    fixture.seed("later", "18446744073709551615");
    auto first = authority::catalog_float_prefix(*fixture.connection,
                                                 fixture.auth, "text/plain", 1);
    REQUIRE(first);
    REQUIRE(first->targets.size() == 1);
    REQUIRE(first->targets[0].application_id == "earliest");
    REQUIRE(first->next_position);
    REQUIRE(first->next_position->logical_install_order == "9007199254740992");
    auto second =
        authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                        "text/plain", 1, first->next_position);
    REQUIRE(second);
    REQUIRE(second->targets[0].application_id == "notes");
    auto last =
        authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                        "text/plain", 1, second->next_position);
    REQUIRE(last);
    REQUIRE(last->targets[0].application_id == "later");
    REQUIRE_FALSE(last->next_position);
    fixture.run("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT id FROM "
                "plinth.rbac_rules WHERE rule='earliest.call')");
    first = authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                            "text/plain", 1);
    REQUIRE(first);
    REQUIRE(first->targets[0].application_id == "notes");
    REQUIRE(authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                            "text/plain", 65)
                .error() == authority::Error::INVALID);
    REQUIRE(authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                            "Text/Plain", 1)
                .error() == authority::Error::INVALID);
    auto unfiltered = authority::catalog_float_prefix(
        *fixture.connection, fixture.auth, std::nullopt, 64);
    REQUIRE(unfiltered);
    REQUIRE(unfiltered->targets.size() == 2);
  }
  SECTION("missing ordinal never becomes an invisible earlier candidate") {
    fixture.run("UPDATE plinth.panels SET logical_install_order=NULL");
    auto page = authority::catalog_float_prefix(*fixture.connection,
                                                fixture.auth, "text/plain", 1);
    REQUIRE_FALSE(page);
    REQUIRE(page.error() == authority::Error::UNAVAILABLE);
    const authority::Position after{.logical_install_order =
                                        "18446744073709551615",
                                    .application_id = "notes",
                                    .panel_id = "preview"};
    page = authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                           "text/plain", 1, after);
    REQUIRE_FALSE(page);
    REQUIRE(page.error() == authority::Error::UNAVAILABLE);
  }
  SECTION(
      "specific group membership and grants are fresh in lookup and catalog") {
    fixture.run(
        "DELETE FROM plinth.group_rules WHERE rule_id IN (SELECT id FROM "
        "plinth.rbac_rules WHERE rule IN ('notes.panel','notes.call'))");
    fixture.run("INSERT INTO plinth.groups(name,description) "
                "VALUES('surface-specific','fixture')");
    fixture.run(
        "INSERT INTO plinth.group_members(group_id,user_id) SELECT id,$1::uuid "
        "FROM plinth.groups WHERE name='surface-specific'",
        {USER});
    fixture.run(
        "INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
        "FROM plinth.groups g,plinth.rbac_rules r WHERE "
        "g.name='surface-specific' "
        "AND r.rule IN ('notes.panel','notes.call')");
    auto catalog = [&] {
      return authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                             "text/plain", 64);
    };
    auto admitted = fixture.lookup();
    auto page = catalog();
    REQUIRE(admitted);
    REQUIRE(admitted->target);
    REQUIRE(page);
    REQUIRE(page->targets.size() == 1);
    fixture.run("DELETE FROM plinth.group_members WHERE user_id=$1::uuid",
                {USER});
    auto missing_membership = fixture.lookup();
    page = catalog();
    REQUIRE(missing_membership);
    REQUIRE_FALSE(missing_membership->target);
    REQUIRE(page);
    REQUIRE(page->targets.empty());
    fixture.run(
        "INSERT INTO plinth.group_members(group_id,user_id) SELECT id,$1::uuid "
        "FROM plinth.groups WHERE name='surface-specific'",
        {USER});
    admitted = fixture.lookup();
    page = catalog();
    REQUIRE(admitted);
    REQUIRE(admitted->target);
    REQUIRE(page);
    REQUIRE(page->targets.size() == 1);
    fixture.run("DELETE FROM plinth.group_rules WHERE rule_id=(SELECT id FROM "
                "plinth.rbac_rules WHERE rule='notes.call')");
    auto missing_grant = fixture.lookup();
    page = catalog();
    REQUIRE(missing_grant);
    REQUIRE_FALSE(missing_grant->target);
    REQUIRE(page);
    REQUIRE(page->targets.empty());
    fixture.run(
        "INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id "
        "FROM plinth.groups g,plinth.rbac_rules r WHERE "
        "g.name='surface-specific' "
        "AND r.rule='notes.call'");
    admitted = fixture.lookup();
    page = catalog();
    REQUIRE(admitted);
    REQUIRE(admitted->target);
    REQUIRE(page);
    REQUIRE(page->targets.size() == 1);
    fixture.run("UPDATE plinth.sessions SET revoked_at=clock_timestamp() WHERE "
                "id=$1::uuid",
                {SESSION});
    auto revoked = fixture.lookup();
    page = catalog();
    REQUIRE_FALSE(revoked);
    REQUIRE(revoked.error() == authority::Error::UNAUTHENTICATED);
    REQUIRE_FALSE(page);
    REQUIRE(page.error() == authority::Error::UNAUTHENTICATED);
  }
  SECTION("lookup and catalog preserve unknown database authority failures") {
    fixture.run("BEGIN");
    const Result failed{PQexec(fixture.connection.get(), "SELECT 1/0"),
                        PQclear};
    REQUIRE(PQresultStatus(failed.get()) == PGRES_FATAL_ERROR);
    const auto lookup = fixture.lookup();
    const auto page = authority::catalog_float_prefix(
        *fixture.connection, fixture.auth, "text/plain", 1);
    REQUIRE_FALSE(lookup);
    REQUIRE(lookup.error() == authority::Error::UNAVAILABLE);
    REQUIRE_FALSE(page);
    REQUIRE(page.error() == authority::Error::UNAVAILABLE);
    fixture.run("ROLLBACK");
  }
  SECTION(
      "equal ordinals use exact app and panel byte ties through cursor end") {
    fixture.seed("no0tes", "1", "zeta");
    fixture.seed("not0es", "1");
    fixture.run(
        "INSERT INTO plinth.panels(package_id,panel_id,panel_type,declaration,"
        "validated_format_version,logical_install_order) SELECT pn.package_id,"
        "'pre-view','float',jsonb_set(pn.declaration,'{id}','\"pre-view\"'),2,"
        "1 "
        "FROM plinth.panels pn JOIN plinth.packages p ON p.id=pn.package_id "
        "WHERE p.name='notes' AND pn.panel_id='preview'");
    const std::vector<std::pair<std::string, std::string>> expected{
        {"no0tes", "zeta"},
        {"not0es", "preview"},
        {"notes", "pre-view"},
        {"notes", "preview"}};
    std::optional<authority::Position> after;
    for (std::size_t index = 0; index < expected.size(); ++index) {
      const auto page = authority::catalog_float_prefix(
          *fixture.connection, fixture.auth, "text/plain", 1, after);
      REQUIRE(page);
      REQUIRE(page->targets.size() == 1);
      REQUIRE(page->targets[0].application_id == expected[index].first);
      REQUIRE(page->targets[0].declaration.id == expected[index].second);
      const authority::Position position{.logical_install_order = "1",
                                         .application_id =
                                             expected[index].first,
                                         .panel_id = expected[index].second};
      if (index + 1 < expected.size()) {
        REQUIRE(page->next_position);
        REQUIRE(*page->next_position == position);
      } else {
        REQUIRE_FALSE(page->next_position);
      }
      after = position;
    }
    const auto exhausted = authority::catalog_float_prefix(
        *fixture.connection, fixture.auth, "text/plain", 1, after);
    REQUIRE(exhausted);
    REQUIRE(exhausted->targets.empty());
    REQUIRE_FALSE(exhausted->next_position);
  }
  SECTION(
      "maximum catalog page has one lookahead and a bounded terminal cursor") {
    for (unsigned int index = 1; index <= 64; ++index)
      fixture.seed("paged" + std::to_string(index), std::to_string(index + 1));
    const auto first = authority::catalog_float_prefix(
        *fixture.connection, fixture.auth, "text/plain", 64);
    REQUIRE(first);
    REQUIRE(first->targets.size() == 64);
    REQUIRE(first->targets[0].application_id == "notes");
    for (std::size_t index = 1; index < 64; ++index) {
      REQUIRE(first->targets[index].application_id ==
              "paged" + std::to_string(index));
      REQUIRE(first->targets[index].logical_install_order ==
              std::to_string(index + 1));
    }
    REQUIRE(first->next_position);
    REQUIRE(first->next_position->logical_install_order == "64");
    REQUIRE(first->next_position->application_id == "paged63");
    const auto last =
        authority::catalog_float_prefix(*fixture.connection, fixture.auth,
                                        "text/plain", 64, first->next_position);
    REQUIRE(last);
    REQUIRE(last->targets.size() == 1);
    REQUIRE(last->targets[0].application_id == "paged64");
    REQUIRE(last->targets[0].logical_install_order == "65");
    REQUIRE_FALSE(last->next_position);
    const authority::Position end{.logical_install_order = "65",
                                  .application_id = "paged64",
                                  .panel_id = "preview"};
    const auto exhausted = authority::catalog_float_prefix(
        *fixture.connection, fixture.auth, "text/plain", 64, end);
    REQUIRE(exhausted);
    REQUIRE(exhausted->targets.empty());
    REQUIRE_FALSE(exhausted->next_position);
  }
}
