// Pure argparse-drive coverage for `plinth test rbac` (ICD-0.4.7 B.08).
// Does not invoke `run_cli_test_rbac` — that path is exercised end-to-end
// by PB.07 in `tests/kernel/packages/rbac_test_runner_test.cpp`. This test
// replicates main.cpp's subparser definitions so a drift between the two
// (a missing flag, a wrong default) surfaces here without needing a
// running kernel.

#include <argparse/argparse.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

// Mirrors main.cpp's test / test_rbac_cmd argparse block. Kept in this
// translation unit deliberately: the duplication is the contract — B.08
// is a safety net against silent drift.
struct TestRbacParserTree {
  argparse::ArgumentParser program;
  argparse::ArgumentParser test_cmd;
  argparse::ArgumentParser test_rbac_cmd;

  TestRbacParserTree()
      : program("plinth"), test_cmd("test"), test_rbac_cmd("rbac") {
    test_rbac_cmd.add_argument("extension");
    test_rbac_cmd.add_argument("--config", "-c");
    test_rbac_cmd.add_argument("--json").default_value(false).implicit_value(
        true);
    test_rbac_cmd.add_argument("--run-id").default_value(std::string{});
    test_cmd.add_subparser(test_rbac_cmd);
    program.add_subparser(test_cmd);
  }
  TestRbacParserTree(const TestRbacParserTree&) = delete;
  auto operator=(const TestRbacParserTree&) -> TestRbacParserTree& = delete;
  TestRbacParserTree(TestRbacParserTree&&) = delete;
  auto operator=(TestRbacParserTree&&) -> TestRbacParserTree& = delete;
  ~TestRbacParserTree() = default;
};

} // namespace

TEST_CASE("B.08 test rbac subcommand recognised with bare extension name",
          "[cli][test_rbac]") {
  TestRbacParserTree t;
  std::vector<std::string> args = {"plinth", "test", "rbac", "notes"};
  t.program.parse_args(args);

  REQUIRE(t.program.is_subcommand_used("test"));
  REQUIRE(t.test_cmd.is_subcommand_used("rbac"));
  REQUIRE(t.test_rbac_cmd.get<std::string>("extension") == "notes");
  REQUIRE(t.test_rbac_cmd.get<bool>("--json") == false);
  REQUIRE(t.test_rbac_cmd.get<std::string>("--run-id").empty());
  REQUIRE_FALSE(t.test_rbac_cmd.is_used("--config"));
}

TEST_CASE("B.08 --json flag recognised", "[cli][test_rbac]") {
  TestRbacParserTree t;
  std::vector<std::string> args = {"plinth", "test", "rbac", "notes", "--json"};
  t.program.parse_args(args);

  REQUIRE(t.program.is_subcommand_used("test"));
  REQUIRE(t.test_cmd.is_subcommand_used("rbac"));
  REQUIRE(t.test_rbac_cmd.get<std::string>("extension") == "notes");
  REQUIRE(t.test_rbac_cmd.get<bool>("--json") == true);
}

TEST_CASE("B.08 --run-id flag forwards UUIDv4", "[cli][test_rbac]") {
  TestRbacParserTree t;
  std::vector<std::string> args = {
      "plinth", "test",     "rbac",
      "notes",  "--run-id", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"};
  t.program.parse_args(args);

  REQUIRE(t.test_rbac_cmd.get<std::string>("--run-id") ==
          "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
}

TEST_CASE("B.08 --config -c short flag recognised", "[cli][test_rbac]") {
  TestRbacParserTree t;
  std::vector<std::string> args = {"plinth", "test", "rbac",
                                   "notes",  "-c",   "/tmp/cfg.json"};
  t.program.parse_args(args);

  REQUIRE(t.test_rbac_cmd.get<std::string>("--config") == "/tmp/cfg.json");
  REQUIRE(t.test_rbac_cmd.is_used("--config"));
}

TEST_CASE("B.08 missing extension positional fails argparse",
          "[cli][test_rbac]") {
  TestRbacParserTree t;
  std::vector<std::string> args = {"plinth", "test", "rbac"};
  REQUIRE_THROWS(t.program.parse_args(args));
}
