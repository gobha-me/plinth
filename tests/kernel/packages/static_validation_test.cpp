#include "static_validation_fixture.hpp"

#include "kernel/packages/migrations_internal.hpp"

#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <utility>

namespace fs = std::filesystem;
namespace static_test = plinth::packages::static_test;
using plinth::packages::Phase;
using plinth::packages::ValidationConfig;
using plinth::packages::ValidationReport;

namespace {

auto has_rule(const ValidationReport& report, std::string_view code) -> bool {
  return std::ranges::any_of(report.messages, [&](const auto& message) {
    return message.rule == code;
  });
}

auto render(const ValidationReport& report, const fs::path& root, bool json)
    -> std::string {
  std::ostringstream output;
  if (json) {
    plinth::packages::render_json(report, root, output);
  } else {
    plinth::packages::render_text(report, root, output, {});
  }
  return output.str();
}

} // namespace

TEST_CASE("static validation: typed RBAC diagnostics supplement legacy CF",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  const auto call =
      GENERATE("notes:1:read", "not a valid signature", "notes:99:missing");
  copy.assertion(call);
  const auto report = plinth::packages::validate(copy.root());
  if (std::string_view{call} == "notes:1:read") {
    REQUIRE(report.disposition() == 0);
  } else {
    REQUIRE(report.disposition() == 1);
    const auto expected = std::string_view{call} == "not a valid signature"
                              ? "rbac.test.call.parse_error"
                              : "rbac.test.call.unresolved";
    const auto found = std::ranges::find(
        report.messages, expected, &plinth::packages::ValidationMessage::rule);
    REQUIRE(found != report.messages.end());
    CHECK(found->phase == Phase::CROSS_FILE);
    CHECK(found->path == "rbac.json/rules/0/test/assert_allow/call");
    ValidationConfig structure;
    structure.cross_file = false;
    CHECK(plinth::packages::validate(copy.root(), structure).disposition() ==
          0);
  }
}

TEST_CASE("static validation: RBAC schema errors preserve source indexes",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  auto rbac = copy.json("rbac.json");
  const auto valid = rbac["rules"][0];
  rbac["rules"] = nlohmann::json::array({valid, valid});
  rbac["rules"][0].erase("description");
  rbac["rules"][1]["test"] = {
      {"assert_allow", {{"call", "notes:99:missing"}, {"expect", "success"}}}};
  copy.write("rbac.json", rbac.dump());
  ValidationConfig structure;
  structure.cross_file = false;
  for (const auto& config : {ValidationConfig{}, structure}) {
    const auto report = plinth::packages::validate(copy.root(), config);
    REQUIRE(report.disposition() == 1);
    CHECK(has_rule(report, "rbac.description.missing"));
    CHECK_FALSE(has_rule(report, "rbac.test.call.unresolved"));
    const auto found =
        std::ranges::find(report.messages, "rbac.description.missing",
                          &plinth::packages::ValidationMessage::rule);
    REQUIRE(found != report.messages.end());
    CHECK(found->path == "rbac.json/rules/0/description");
    CHECK(found->phase == Phase::STRUCTURE);
  }
}

TEST_CASE("static validation: malformed RBAC JSON keeps json-structure",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  copy.write("rbac.json", "{");
  const auto report = plinth::packages::validate(copy.root());
  CHECK(has_rule(report, "json-structure"));
  CHECK_FALSE(has_rule(report, "rbac.json.parse_error"));
}

TEST_CASE("static validation: migration discovery shares existing diagnostics",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  const auto scenario = GENERATE("valid", "absent", "empty", "duplicate",
                                 "filename", "gap", "ignored", "unqualified");
  const auto directory = copy.root() / "migrations";
  const auto file = directory / "001_init.sql";
  std::string expected;
  if (std::string_view{scenario} == "absent") {
    fs::remove_all(directory);
  } else if (std::string_view{scenario} == "empty") {
    fs::remove(file);
  } else if (std::string_view{scenario} == "duplicate") {
    fs::copy_file(file, directory / "001_duplicate.sql");
    expected = "migration-duplicate-sequence";
  } else if (std::string_view{scenario} == "filename") {
    fs::rename(file, directory / "001.sql");
    expected = "migration-invalid-filename";
  } else if (std::string_view{scenario} == "gap") {
    fs::copy_file(file, directory / "004_next.sql");
    expected = "migration-gap";
  } else if (std::string_view{scenario} == "ignored") {
    copy.write("migrations/README.md", "Ordinary fixture notes.\n");
  } else if (std::string_view{scenario} == "unqualified") {
    // Reuse migrations_parsing_test's ordinary qualified-DDL policy case.
    copy.write("migrations/001_init.sql", "CREATE TABLE notes (id INT);");
    expected = "migration-unqualified-ddl";
  }
  const auto report = plinth::packages::validate(copy.root());
  if (std::string_view{scenario} == "absent") {
    REQUIRE(report.disposition() == 0);
    return;
  }
  const auto discovered =
      plinth::packages::detail::discover_migrations(directory, "notes");
  if (expected.empty()) {
    REQUIRE(discovered.has_value());
    CHECK(report.disposition() == 0);
  } else {
    REQUIRE(has_rule(report, expected));
    const auto found = std::ranges::find(
        report.messages, expected, &plinth::packages::ValidationMessage::rule);
    REQUIRE(found != report.messages.end());
    CHECK(found->phase == Phase::CROSS_FILE);
    if (expected == "migration-gap") {
      REQUIRE(discovered.has_value());
      REQUIRE(discovered->warnings.size() == 1);
      CHECK(found->message == discovered->warnings.front().detail);
      CHECK(report.disposition() == 2);
    } else {
      REQUIRE_FALSE(discovered.has_value());
      CHECK(found->message == discovered.error().message);
      REQUIRE(discovered.error().migration_file.has_value());
      CHECK(found->path == "migrations/" + *discovered.error().migration_file);
      CHECK(report.disposition() == 1);
    }
    ValidationConfig structure;
    structure.cross_file = false;
    CHECK_FALSE(
        has_rule(plinth::packages::validate(copy.root(), structure), expected));
  }
}

TEST_CASE("static validation: required parse errors skip migration semantics",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  const auto capabilities = copy.json("capabilities.json").dump();
  fs::copy_file(copy.root() / "migrations/001_init.sql",
                copy.root() / "migrations/001_duplicate.sql");
  fs::copy_file(
      static_test::fixture("packages/bad-capabilities-json/capabilities.json"),
      copy.root() / "capabilities.json", fs::copy_options::overwrite_existing);
  const auto skipped = plinth::packages::validate(copy.root());
  REQUIRE(skipped.disposition() == 1);
  CHECK(has_rule(skipped, "capabilities.provides[0].function.missing"));
  CHECK(has_rule(skipped, "cross-file-skipped"));
  CHECK(std::ranges::none_of(skipped.messages, [](const auto& message) {
    return message.rule.starts_with("migration-");
  }));
  const auto structural = std::ranges::find(
      skipped.messages, "capabilities.provides[0].function.missing",
      &plinth::packages::ValidationMessage::rule);
  REQUIRE(structural != skipped.messages.end());
  CHECK(structural->phase == Phase::STRUCTURE);
  CHECK(structural->severity == plinth::packages::Severity::ERROR);
  copy.write("capabilities.json", capabilities);
  const auto complete = plinth::packages::validate(copy.root());
  CHECK(has_rule(complete, "migration-duplicate-sequence"));
  CHECK_FALSE(has_rule(complete, "cross-file-skipped"));
}

TEST_CASE("static validation: finding order ignores file creation order",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  const std::array<std::string_view, 3> files{
      "z-unexpected.txt", "a-unexpected.txt", "m-unexpected.txt"};
  const auto write_files = [&](bool reverse) {
    for (std::size_t i = 0; i < files.size(); ++i) {
      copy.write(files[reverse ? files.size() - i - 1 : i], "fixture\n");
    }
  };
  write_files(false);
  const auto first = plinth::packages::validate(copy.root());
  const auto json = render(first, copy.root(), true);
  const auto text = render(first, copy.root(), false);
  for (const auto file : files)
    fs::remove(copy.root() / file);
  write_files(true);
  const auto second = plinth::packages::validate(copy.root());
  CHECK(render(second, copy.root(), true) == json);
  CHECK(render(second, copy.root(), false) == text);
  const auto document = nlohmann::json::parse(json);
  REQUIRE(document["messages"].size() == 3);
  CHECK(document["messages"][0]["path"] == "a-unexpected.txt");
  CHECK(document["messages"][1]["path"] == "m-unexpected.txt");
  CHECK(document["messages"][2]["path"] == "z-unexpected.txt");
  CHECK(document["exit_code"] == 1);
  CHECK(document["total_bytes"] == copy.tree_bytes());
  CHECK(json.ends_with('\n'));
  CHECK(json.find('\x1b') == std::string::npos);
  for (const auto& message : document["messages"]) {
    CHECK(message["phase"] == "structure");
    CHECK(message["severity"] == "error");
    CHECK(message["rule"] == "forbidden-paths");
    CHECK(message.contains("remediation"));
  }
}

TEST_CASE("static validation: appended passes preserve legacy finding order",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  copy.assertion("notes:99:missing");
  auto rbac = copy.json("rbac.json");
  rbac["rules"][0]["namespace"] = "other";
  copy.write("rbac.json", rbac.dump());
  fs::copy_file(copy.root() / "migrations/001_init.sql",
                copy.root() / "migrations/001_zduplicate.sql");
  const auto report = plinth::packages::validate(copy.root());
  const std::array<std::pair<std::string_view, std::string_view>, 5> expected{{
      {"rbac-orphan-namespace", "rbac.json"},
      {"capability-without-rule", "capabilities.json"},
      {"rbac.rule.namespace_mismatch", "rbac.json/rules/0/namespace"},
      {"rbac.test.call.unresolved", "rbac.json/rules/0/test/assert_allow/call"},
      {"migration-duplicate-sequence", "migrations/001_zduplicate.sql"},
  }};
  REQUIRE(report.messages.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(report.messages[i].rule == expected[i].first);
    CHECK(report.messages[i].path == expected[i].second);
    CHECK(report.messages[i].phase == Phase::CROSS_FILE);
    CHECK(report.messages[i].severity ==
          (i == 1 ? plinth::packages::Severity::WARNING
                  : plinth::packages::Severity::ERROR));
  }
}

TEST_CASE("static validation: Unicode ordering retains every existing pass",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  const auto source = static_test::fixture(
      "packages/unicode-smuggle-variation-selectors/server/handlers/shell.js");
  const std::array<std::string_view, 7> files{"server/handlers/z.js",
                                              "server/handlers/a.js",
                                              "server/handlers/.git/reused.js",
                                              "client/panels/z.js",
                                              "client/panels/a.js",
                                              "client/panels/.git/reused.js",
                                              "client/components/a.js"};
  const auto copy_files = [&](bool reverse) {
    for (std::size_t i = 0; i < files.size(); ++i) {
      const auto path = copy.root() / files[reverse ? files.size() - i - 1 : i];
      fs::create_directories(path.parent_path());
      fs::copy_file(source, path);
    }
  };
  auto manifest = copy.json("manifest.json");
  manifest["entry_point"] = "server/handlers/a.js";
  copy.write("manifest.json", manifest.dump());
  ValidationConfig config;
  config.unicode_scanner_log_findings = false;
  copy_files(false);
  const auto first = plinth::packages::validate(copy.root(), config);
  for (const auto file : files)
    fs::remove(copy.root() / file);
  copy_files(true);
  const auto second = plinth::packages::validate(copy.root(), config);
  CHECK(render(first, copy.root(), true) == render(second, copy.root(), true));
  const std::array<std::string_view, 8> expected{
      "server/handlers/.git/reused.js",
      "server/handlers/a.js",
      "server/handlers/z.js",
      "server/handlers/a.js",
      "client/panels/.git/reused.js",
      "client/panels/a.js",
      "client/panels/z.js",
      "client/components/a.js"};
  REQUIRE(first.messages.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(first.messages[i].rule == "unicode-smuggle");
    CHECK(first.messages[i].path == expected[i]);
    CHECK(first.messages[i].phase == Phase::STRUCTURE);
    CHECK(first.messages[i].severity == plinth::packages::Severity::ERROR);
  }
}

TEST_CASE("static validation: exact tree size is inclusive",
          "[packages][validator][static-parity]") {
  static_test::PackageCopy copy;
  ValidationConfig config;
  config.max_size_bytes = copy.tree_bytes();
  CHECK(plinth::packages::validate(copy.root(), config).disposition() == 0);
  --config.max_size_bytes;
  const auto rejected = plinth::packages::validate(copy.root(), config);
  CHECK(rejected.disposition() == 1);
  CHECK(std::ranges::count(rejected.messages, "size-limit",
                           &plinth::packages::ValidationMessage::rule) == 1);
}
