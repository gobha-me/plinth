#include "kernel/packages/generation_binding.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/frontend/surface_authority.hpp"
#include "kernel/packages/migrations_internal.hpp"
#include "kernel/packages/panels_manifest.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace generation = plinth::packages::generation;
namespace names = plinth::packages::name_operation;
namespace fs = std::filesystem;
using BindingJson = nlohmann::json;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

struct Tree {
  Tree() {
    std::array<char, 64> pattern{};
    std::ranges::copy(std::string{"/tmp/plinth-generation-binding.XXXXXX"},
                      pattern.begin());
    const auto* path = ::mkdtemp(pattern.data());
    if (!path) throw std::runtime_error("binding fixture directory");
    root = path;
    fs::create_directories(root / "client/panels");
    fs::create_directories(root / "server");
  }
  ~Tree() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  Tree(const Tree&) = delete;
  auto operator=(const Tree&) -> Tree& = delete;
  auto write(std::string_view path, std::string_view bytes) const -> void {
    std::ofstream file{root / path, std::ios::binary | std::ios::trunc};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
  }
  fs::path root;
};

struct Fixture {
  Tree tree;
  plinth::Config config{plinth::ws_test::test_config()};
  generation::Budget budget;
  std::shared_ptr<PGconn> connection;
  std::optional<names::NameOperation> operation;
  generation::GenerationKey key{.package_id =
                                    "10000000-0000-4000-8000-000000000046",
                                .name = "notes",
                                .version = "1.0.0",
                                .profile = 1};
  BindingJson manifest{
      {"name", "notes"},          {"version", "1.0.0"},
      {"description", "fixture"}, {"author", "fixture"},
      {"license", "MIT"},         {"entry_point", "server/main.js"}};
  BindingJson capability{
      {"namespace", "notes"},     {"version", 1},
      {"function", "preview"},    {"params", BindingJson::array()},
      {"returns", "object"},      {"scope", "instance"},
      {"description", "fixture"}, {"rbac_rule", "notes.call"}};
  BindingJson panel{
      {"id", "preview"},
      {"type", "float"},
      {"client_path", "preview.js"},
      {"rbac_rule", "notes.panel"},
      {"capability", "notes:1:preview"},
      {"content_types", BindingJson::array({"text/plain"})},
      {"context_schema",
       {{"version", 1},
        {"fields",
         {{"record_id",
           {{"required", true}, {"max_bytes", 32}, {"meaning", "stable ID"}}}}},
        {"persist_keys", BindingJson::array({"record_id"})}}}};
  std::shared_ptr<const generation::CapturedArtifact> artifact;

  Fixture() {
    plinth::ws_test::reset_schema(config.db);
    connection = std::shared_ptr<PGconn>{
        plinth::db::connect(plinth::db::connection_info(config.db).c_str()),
        PQfinish};
    REQUIRE(connection);
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
    tree.write("server/main.js", "export default {};\n");
    tree.write("client/panels/preview.js",
               "export default function Preview() {}\n");
    tree.write("manifest.json", manifest.dump());
    tree.write("capabilities.json",
               BindingJson{{"provides", BindingJson::array({capability})},
                           {"requires", BindingJson::array()}}
                   .dump());
    const auto document = BindingJson{{"format_version", 2},
                                      {"panels", BindingJson::array({panel})}}
                              .dump();
    tree.write("panels.json", document);
    const auto parsed =
        plinth::packages::PanelsManifest::parse(document, "panels.json");
    REQUIRE(parsed.value);
    REQUIRE(parsed.value->panels.size() == 1);
    run("INSERT INTO "
        "plinth.packages(id,name,version,state,provenance,manifest_json,"
        "entry_point,manifest_checksum) "
        "VALUES($1::uuid,'notes','1.0.0','ACTIVATING',"
        "'user',$2::jsonb,'server/main.js',$3)",
        {key.package_id, manifest.dump(),
         plinth::packages::detail::sha256_hex(manifest.dump())});
    run("INSERT INTO "
        "plinth.rbac_rules(rule,namespace,description,extension_name) "
        "VALUES('notes.panel','notes','fixture','notes'),('notes.call','notes',"
        "'fixture','notes')");
    run("INSERT INTO "
        "plinth.capabilities(namespace,version,function,signature,scope,"
        "description,rbac_rule,enabled,provider_type,extension_name) "
        "VALUES('notes',1,"
        "'preview','notes:1:preview','instance','fixture','notes.call',true,'"
        "extension','notes')");
    run("INSERT INTO "
        "plinth.panels(package_id,panel_id,panel_type,validated_format_version,"
        "declaration,logical_install_order) "
        "VALUES($1::uuid,'preview','float',2,$2::jsonb,1)",
        {key.package_id, parsed.value->panels[0].declaration().dump()});
    auto acquired = names::NameOperation::acquire(connection, key.name);
    REQUIRE(acquired);
    operation.emplace(std::move(*acquired));
    recapture();
  }
  ~Fixture() {
    operation.reset();
    plinth::ws_test::settle_fixture_work_or_exit();
  }
  Fixture(const Fixture&) = delete;
  auto operator=(const Fixture&) -> Fixture& = delete;
  auto run(const std::string& sql, const std::vector<std::string>& values = {})
      -> void {
    std::vector<const char*> pointers;
    pointers.reserve(values.size());
    for (const auto& value : values)
      pointers.push_back(value.c_str());
    const Result result{
        plinth::db::exec_params(connection.get(), sql.c_str(),
                                static_cast<int>(pointers.size()), nullptr,
                                pointers.data(), nullptr, nullptr, 0),
        PQclear};
    REQUIRE(result);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto recapture() -> void {
    const generation::CaptureRequest request{
        .key = key,
        .root = tree.root,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10},
        .cancellation = {}};
    auto captured = generation::capture_artifact(request, budget);
    REQUIRE(captured);
    artifact = std::move(*captured);
  }
  auto bind() -> std::expected<std::string, generation::BindingError> {
    return generation::bind_registered_generation(*operation, *artifact,
                                                  budget);
  }
  auto update_manifest() -> void {
    const auto bytes = manifest.dump();
    tree.write("manifest.json", bytes);
    run("UPDATE plinth.packages SET "
        "manifest_json=$1::jsonb,manifest_checksum=$2",
        {bytes, plinth::packages::detail::sha256_hex(bytes)});
    recapture();
  }
};
} // namespace

TEST_CASE("generation binding: exact captured and registered inventory",
          "[packages][generation_binding][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  Fixture fixture;
  const auto used = fixture.budget.used();
  const auto first = fixture.bind();
  REQUIRE(first);
  REQUIRE(first->size() == 64);
  REQUIRE(first->find_first_not_of("0123456789abcdef") == std::string::npos);
  REQUIRE(fixture.budget.used() == used);
  REQUIRE(PQtransactionStatus(fixture.connection.get()) == PQTRANS_IDLE);

  SECTION("deterministic read owns no transaction and performs no filesystem "
          "reads") {
    std::error_code error;
    fs::remove_all(fixture.tree.root, error);
    REQUIRE_FALSE(error);
    REQUIRE(fixture.bind() == first);
    fixture.run("BEGIN");
    REQUIRE(fixture.bind() == first);
    REQUIRE(PQtransactionStatus(fixture.connection.get()) == PQTRANS_INTRANS);
    fixture.run("ROLLBACK");
    REQUIRE(fixture.budget.used() == used);
  }
  SECTION("working allocation is charged before parsing or queries") {
    generation::Budget no_working_memory{0};
    const auto refused = generation::bind_registered_generation(
        *fixture.operation, *fixture.artifact, no_working_memory);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::LIMIT_EXCEEDED);
    REQUIRE(no_working_memory.used() == 0);
  }
  SECTION("lost session lock is not replaced or reacquired") {
    REQUIRE(names::release_seed_checked(fixture.connection.get(),
                                        names::lock_key("notes")));
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::LOCK_NOT_OWNED);
    REQUIRE(names::try_acquire_seed(fixture.connection.get(),
                                    names::lock_key("notes")));
  }
  SECTION("exact generation and stored captured manifest remain bound") {
    SECTION("name mismatch") {
      fixture.run("UPDATE plinth.packages SET name='renamed'");
    }
    SECTION("version mismatch") {
      fixture.run("UPDATE plinth.packages SET version='2.0.0'");
    }
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::GENERATION_MISMATCH);
  }
  SECTION("all declared panel registration fields are exact") {
    SECTION("missing row") {
      fixture.run("DELETE FROM plinth.panels");
    }
    SECTION("changed declaration") {
      fixture.run("UPDATE plinth.panels SET "
                  "declaration=jsonb_set(declaration,'{order}','1')");
    }
    SECTION("missing assigned logical order") {
      fixture.run("UPDATE plinth.panels SET logical_install_order=NULL");
    }
    SECTION("changed stored format marker") {
      fixture.run("UPDATE plinth.panels SET "
                  "logical_install_order=NULL,validated_format_version=0");
    }
    SECTION("foreign rule owner") {
      fixture.run("UPDATE plinth.rbac_rules SET extension_name='foreign' WHERE "
                  "rule='notes.panel'");
    }
    SECTION("orphaned rule") {
      fixture.run("UPDATE plinth.rbac_rules SET orphaned_at=clock_timestamp() "
                  "WHERE rule='notes.panel'");
    }
    SECTION("extra row") {
      fixture.run(
          "INSERT INTO "
          "plinth.panels(package_id,panel_id,panel_type,validated_format_"
          "version,"
          "declaration,logical_install_order) SELECT "
          "package_id,'extra','float',2,declaration,2 FROM plinth.panels");
    }
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::DECLARATION_MISMATCH);
  }
  SECTION(
      "logical order is canonical exact uint64 and included in the digest") {
    fixture.run(
        "UPDATE plinth.panels SET logical_install_order=18446744073709551615");
    const auto changed = fixture.bind();
    REQUIRE(changed);
    REQUIRE(*changed != *first);
    REQUIRE(fixture.bind() == changed);
  }
  SECTION("all provided capability registrations and ownership are exact") {
    SECTION("missing") {
      fixture.run("DELETE FROM plinth.capabilities WHERE namespace='notes'");
    }
    SECTION("disabled") {
      fixture.run("UPDATE plinth.capabilities SET enabled=false WHERE "
                  "namespace='notes'");
    }
    SECTION("foreign provider") {
      fixture.run("UPDATE plinth.capabilities SET extension_name='foreign' "
                  "WHERE namespace='notes'");
    }
    SECTION("kernel provider substitution") {
      fixture.run("UPDATE plinth.capabilities SET provider_type='kernel' WHERE "
                  "namespace='notes'");
    }
    SECTION("wrong signature") {
      fixture.run("UPDATE plinth.capabilities SET signature='notes:1:other' "
                  "WHERE namespace='notes'");
    }
    SECTION("description drift") {
      fixture.run("UPDATE plinth.capabilities SET description='changed' WHERE "
                  "namespace='notes'");
    }
    SECTION("foreign rule owner") {
      fixture.run("UPDATE plinth.rbac_rules SET extension_name='foreign' WHERE "
                  "rule='notes.call'");
    }
    SECTION("orphaned rule") {
      fixture.run("UPDATE plinth.rbac_rules SET orphaned_at=clock_timestamp() "
                  "WHERE rule='notes.call'");
    }
    SECTION("unsupported user row shadows instance") {
      fixture.run("INSERT INTO "
                  "plinth.capabilities(namespace,version,function,signature,"
                  "scope,description,"
                  "rbac_rule,enabled,provider_type,extension_name) "
                  "VALUES('notes',1,'preview','notes:1:preview',"
                  "'user','fixture','notes.call',false,'extension','foreign')");
    }
    SECTION("foreign namespace still collides on signature") {
      fixture.run("INSERT INTO "
                  "plinth.capabilities(namespace,version,function,signature,"
                  "description,"
                  "rbac_rule,provider_type,extension_name) "
                  "VALUES('foreign',1,'preview','notes:1:preview',"
                  "'fixture','notes.call','extension','foreign')");
    }
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::DECLARATION_MISMATCH);
  }
  SECTION("missing captured panel file cannot borrow a mutable path") {
    fs::remove(fixture.tree.root / "client/panels/preview.js");
    fixture.recapture();
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::DECLARATION_MISMATCH);
  }
  SECTION("declared user capability inventory does not invent user target "
          "authority") {
    // Retained/directly seeded metadata control ONLY. The ordinary capability
    // registration API currently rejects user scope; this is not install proof.
    auto unrelated = fixture.capability;
    unrelated["function"] = "other";
    unrelated["scope"] = "user";
    fixture.tree.write(
        "capabilities.json",
        BindingJson{
            {"provides", BindingJson::array({fixture.capability, unrelated})},
            {"requires", BindingJson::array()}}
            .dump());
    fixture.run(
        "INSERT INTO "
        "plinth.capabilities(namespace,version,function,signature,scope,"
        "description,rbac_rule,provider_type,extension_name) "
        "VALUES('notes',1,'other',"
        "'notes:1:other','user','fixture','notes.call','extension','notes')");
    fixture.recapture();
    REQUIRE(fixture.bind());
    auto user_panel = fixture.panel;
    user_panel["id"] = "user-preview";
    user_panel["capability"] = "notes:1:other";
    const auto panel_bytes =
        BindingJson{{"format_version", 2},
                    {"panels", BindingJson::array({fixture.panel, user_panel})}}
            .dump();
    const auto parsed =
        plinth::packages::PanelsManifest::parse(panel_bytes, "panels.json");
    REQUIRE(parsed.value);
    REQUIRE(parsed.value->panels.size() == 2);
    fixture.tree.write("panels.json", panel_bytes);
    fixture.run(
        "INSERT INTO "
        "plinth.panels(package_id,panel_id,panel_type,validated_format_version,"
        "declaration,logical_install_order) "
        "VALUES($1::uuid,'user-preview','float',2,$2::jsonb,2)",
        {fixture.key.package_id, parsed.value->panels[1].declaration().dump()});
    fixture.recapture();
    REQUIRE(
        fixture.bind()); // Other ineligible metadata cannot ban a generation.
    // A complete declared inventory may contain both scopes for a signature.
    // This binding is not target authorization and creates no user ownership.
    auto shadow = fixture.capability;
    shadow["scope"] = "user";
    fixture.tree.write(
        "capabilities.json",
        BindingJson{{"provides", BindingJson::array(
                                     {fixture.capability, unrelated, shadow})},
                    {"requires", BindingJson::array()}}
            .dump());
    fixture.run(
        "INSERT INTO "
        "plinth.capabilities(namespace,version,function,signature,scope,"
        "description,rbac_rule,provider_type,extension_name) "
        "VALUES('notes',1,'preview',"
        "'notes:1:preview','user','fixture','notes.call','extension','notes')");
    fixture.recapture();
    REQUIRE(fixture.bind());
    constexpr auto user = "20000000-0000-4000-8000-000000000046";
    constexpr auto session = "30000000-0000-4000-8000-000000000046";
    const std::string hash(64, 'a');
    fixture.run("INSERT INTO plinth.users(id,username,password_hash) "
                "VALUES($1::uuid,'binding-user','unused')",
                {user});
    fixture.run("INSERT INTO plinth.sessions(id,user_id,token_hash) "
                "VALUES($1::uuid,$2::uuid,$3)",
                {session, user, hash});
    fixture.run("INSERT INTO plinth.group_rules(group_id,rule_id) SELECT "
                "g.id,r.id FROM "
                "plinth.groups g,plinth.rbac_rules r WHERE g.name='everyone' "
                "AND r.rule IN ('notes.panel','notes.call')");
    fixture.run(
        "UPDATE plinth.packages SET state='ACTIVE',application_ready=true");
    const plinth::auth::AuthContext auth{
        .user_id = user,
        .username = "binding-user",
        .auth_type = "session",
        .session_id = session,
        .pat_id = {},
        .token_hash = hash,
        .credential_source = plinth::auth::CredentialSource::COOKIE};
    auto denied = plinth::frontend::authority::lookup_surface(
        *fixture.connection, auth, "notes", "preview",
        plinth::frontend::SurfaceKind::FLOAT);
    REQUIRE(denied);
    REQUIRE_FALSE(denied->target);
    fixture.run("DELETE FROM plinth.capabilities WHERE "
                "signature='notes:1:preview' AND scope='user'");
    auto admitted = plinth::frontend::authority::lookup_surface(
        *fixture.connection, auth, "notes", "preview",
        plinth::frontend::SurfaceKind::FLOAT);
    REQUIRE(admitted);
    REQUIRE(admitted->target);
    auto user_only = plinth::frontend::authority::lookup_surface(
        *fixture.connection, auth, "notes", "user-preview",
        plinth::frontend::SurfaceKind::FLOAT);
    REQUIRE(user_only);
    REQUIRE_FALSE(user_only->target);
  }
  SECTION("manifest checksum and stored JSON cannot drift") {
    SECTION("checksum") {
      fixture.run(
          "UPDATE plinth.packages SET manifest_checksum=repeat('0',64)");
    }
    SECTION("stored JSON") {
      fixture.run(
          "UPDATE plinth.packages SET "
          "manifest_json=jsonb_set(manifest_json,'{author}','\"other\"')");
    }
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::DECLARATION_MISMATCH);
  }
  SECTION("JSONB unknown-number normalization never rounds integer identity") {
    SECTION("source exponent one and stored integer one agree") {
      fixture.manifest["numeric"] = 1.0;
      auto bytes = fixture.manifest.dump();
      constexpr std::string_view spelling = "\"numeric\":1.0";
      const auto position = bytes.find(spelling);
      REQUIRE(position != std::string::npos);
      bytes.replace(position, spelling.size(), "\"numeric\":1e0");
      fixture.tree.write("manifest.json", bytes);
      fixture.run("UPDATE plinth.packages SET "
                  "manifest_json=$1::jsonb,manifest_checksum=$2",
                  {bytes, plinth::packages::detail::sha256_hex(bytes)});
      fixture.recapture();
      REQUIRE(fixture.bind());
    }
    SECTION("adjacent integer above two-to-53 is not equal to rounded float") {
      fixture.manifest["numeric"] = std::uint64_t{9007199254740993ULL};
      fixture.update_manifest();
      fixture.run(
          "UPDATE plinth.packages SET manifest_json=jsonb_set(manifest_json,"
          "'{numeric}','9007199254740992.0'::jsonb)");
      const auto refused = fixture.bind();
      REQUIRE_FALSE(refused);
      REQUIRE(refused.error() ==
              generation::BindingError::DECLARATION_MISMATCH);
    }
    SECTION("signed lower endpoint is safely representable") {
      fixture.manifest["numeric"] = std::int64_t{-9223372036854775807LL - 1};
      fixture.update_manifest();
      fixture.run(
          "UPDATE plinth.packages SET manifest_json=jsonb_set(manifest_json,"
          "'{numeric}','-9223372036854775808.0'::jsonb)");
      REQUIRE(fixture.bind());
    }
    SECTION("unsigned upper endpoint is exclusive before conversion") {
      fixture.manifest["numeric"] = std::uint64_t{18446744073709551615ULL};
      fixture.update_manifest();
      fixture.run(
          "UPDATE plinth.packages SET manifest_json=jsonb_set(manifest_json,"
          "'{numeric}','18446744073709551616.0'::jsonb)");
      const auto refused = fixture.bind();
      REQUIRE_FALSE(refused);
      REQUIRE(refused.error() ==
              generation::BindingError::DECLARATION_MISMATCH);
    }
  }
  SECTION("new profile bounds rows and database metadata before return") {
    SECTION("combined row inventory lookahead") {
      fixture.run("INSERT INTO "
                  "plinth.panels(package_id,panel_id,panel_type,validated_"
                  "format_version,"
                  "declaration,logical_install_order) SELECT "
                  "p.package_id,'extra-'||g::text,'float',2,"
                  "p.declaration,g+1 FROM plinth.panels p CROSS JOIN "
                  "generate_series(1,4096) g");
    }
    SECTION("oversized stored payload") {
      fixture.run("UPDATE plinth.capabilities SET "
                  "description=repeat('x',1048576) WHERE namespace='notes'");
    }
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::LIMIT_EXCEEDED);
  }
  SECTION("raw captured manifests have independent bounded duplicate-aware "
          "preflight") {
    SECTION("raw aggregate bytes") {
      fixture.tree.write("manifest.json",
                         std::string(std::size_t{1024} * 1024 + 1, ' '));
    }
    SECTION("depth") {
      BindingJson nested = true;
      for (unsigned int index = 0; index < 33; ++index)
        nested = BindingJson{{"child", std::move(nested)}};
      fixture.manifest["unknown"] = std::move(nested);
      fixture.tree.write("manifest.json", fixture.manifest.dump());
    }
    SECTION("nodes") {
      fixture.manifest["unknown"] = BindingJson::array();
      for (unsigned int index = 0; index < 65536; ++index)
        fixture.manifest["unknown"].push_back(0);
      fixture.tree.write("manifest.json", fixture.manifest.dump());
    }
    fixture.recapture();
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::LIMIT_EXCEEDED);
  }
  SECTION("decoded duplicate raw keys cannot silently select a declaration") {
    fixture.tree.write("manifest.json",
                       "{\"name\":\"notes\",\"na\\u006de\":\"notes\"}");
    fixture.recapture();
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::INVALID_CAPTURE);
  }
  SECTION("legacy declarations remain legacy without invented typed markers") {
    const BindingJson legacy_panel{{"id", "preview"},
                                   {"client_path", "preview.js"},
                                   {"rbac_rule", "notes.panel"},
                                   {"type", "float"}};
    const auto bytes =
        BindingJson{{"panels", BindingJson::array({legacy_panel})}}.dump();
    fixture.tree.write("panels.json", bytes);
    const auto parsed =
        plinth::packages::PanelsManifest::parse(bytes, "panels.json");
    REQUIRE(parsed.value);
    REQUIRE_FALSE(parsed.value->format_version);
    fixture.run("UPDATE plinth.panels SET "
                "panel_type='primary',validated_format_version=0,"
                "logical_install_order=NULL,declaration=$1::jsonb",
                {parsed.value->panels[0].declaration().dump()});
    fixture.recapture();
    const auto legacy = fixture.bind();
    REQUIRE(legacy);
    REQUIRE(*legacy != *first);
    fixture.run("UPDATE plinth.panels SET validated_format_version=2");
    const auto invented = fixture.bind();
    REQUIRE_FALSE(invented);
    REQUIRE(invented.error() == generation::BindingError::DECLARATION_MISMATCH);
  }
  SECTION("optional absent panels bind the actual empty registered inventory") {
    fs::remove(fixture.tree.root / "panels.json");
    fixture.run("DELETE FROM plinth.panels");
    fixture.recapture();
    REQUIRE(fixture.bind());
  }
  SECTION("reserved shell parsing takes provenance only from the actual row") {
    REQUIRE(fixture.operation->release_checked());
    fixture.key.name = "shell";
    fixture.manifest["name"] = "shell";
    fixture.capability["namespace"] = "shell";
    fixture.capability["rbac_rule"] = "shell.call";
    fixture.panel["rbac_rule"] = "shell.panel";
    fixture.panel["capability"] = "shell:1:preview";
    fixture.tree.write(
        "capabilities.json",
        BindingJson{{"provides", BindingJson::array({fixture.capability})},
                    {"requires", BindingJson::array()}}
            .dump());
    const auto panel_bytes =
        BindingJson{{"format_version", 2},
                    {"panels", BindingJson::array({fixture.panel})}}
            .dump();
    fixture.tree.write("panels.json", panel_bytes);
    const auto parsed =
        plinth::packages::PanelsManifest::parse(panel_bytes, "panels.json");
    REQUIRE(parsed.value);
    fixture.run("UPDATE plinth.packages SET name='shell'");
    fixture.run(
        "UPDATE plinth.rbac_rules SET rule='shell.panel',namespace='shell',"
        "extension_name='shell' WHERE rule='notes.panel'");
    fixture.run(
        "UPDATE plinth.rbac_rules SET rule='shell.call',namespace='shell',"
        "extension_name='shell' WHERE rule='notes.call'");
    fixture.run("UPDATE plinth.capabilities SET "
                "namespace='shell',signature='shell:1:preview',"
                "rbac_rule='shell.call',extension_name='shell' WHERE "
                "namespace='notes'");
    fixture.run("UPDATE plinth.panels SET declaration=$1::jsonb",
                {parsed.value->panels[0].declaration().dump()});
    fixture.update_manifest();
    auto acquired = names::NameOperation::acquire(fixture.connection, "shell");
    REQUIRE(acquired);
    fixture.operation.emplace(std::move(*acquired));
    const auto reserved = fixture.bind();
    REQUIRE_FALSE(reserved);
    REQUIRE(reserved.error() == generation::BindingError::INVALID_CAPTURE);
    fixture.run("UPDATE plinth.packages SET provenance='bundled'");
    REQUIRE(fixture.bind());
  }
  SECTION("database failure stays unavailable with caller rollback ownership") {
    fixture.run("BEGIN");
    const Result failed{PQexec(fixture.connection.get(), "SELECT 1/0"),
                        PQclear};
    REQUIRE(PQresultStatus(failed.get()) == PGRES_FATAL_ERROR);
    const auto refused = fixture.bind();
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error() == generation::BindingError::UNAVAILABLE);
    REQUIRE(PQtransactionStatus(fixture.connection.get()) == PQTRANS_INERROR);
    fixture.run("ROLLBACK");
  }
}
