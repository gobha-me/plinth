#include "kernel/frontend/persistence_projection.hpp"
#include "kernel/packages/protected_restore_policy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace restore = plinth::packages::restore;

// Set no_new_privs only in an owned child: never change the process-wide
// environment of subsequent PostgreSQL/runtime tests. No background worker is
// started by these cases; all children are reaped before returning.
template <typename Check> auto restricted_child(Check check) -> void {
  const auto child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) ::_exit(2);
    ::_exit(check() ? 0 : 1);
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

} // namespace

TEST_CASE("protected restore policy: absence is not failed configured trust",
          "[packages][protected_restore_policy]") {
  const auto absent = restore::load_protected_policy("");
  REQUIRE(absent.has_value());
  REQUIRE_FALSE(absent->has_value());
  for (const auto path : {"relative.json", "/", "/tmp/test-policy.json",
                          "/proc/self/status", "/etc/../etc/passwd"}) {
    restricted_child([path] {
      const auto loaded = restore::load_protected_policy(path);
      return !loaded && loaded.error() == restore::PolicyLoadError::UNPROTECTED;
    });
  }
  restricted_child([] {
    const auto loaded = restore::load_protected_policy(
        std::string_view{"/etc/policy\0.json", 17});
    return !loaded && loaded.error() == restore::PolicyLoadError::UNPROTECTED;
  });
}

// The dedicated deployment-fixture runner provisions real root-owned files in
// /run and executes the binary as an unprivileged identity. This hidden case is
// explicitly selected by that gate, never silently skipped or faked by chmod.
TEST_CASE("protected restore policy: real protected deployment boundary",
          "[.protected-policy-deployment][packages]") {
  const char* configured = std::getenv("PLINTH_TEST_PROTECTED_POLICY_ROOT");
  REQUIRE(configured != nullptr);
  const std::string root{configured};
  REQUIRE(root.starts_with("/run/plinth-policy-test-"));
  REQUIRE(::geteuid() != 0);
  restricted_child([root] {
    const auto loaded = restore::load_protected_policy(root + "/valid.json");
    if (!loaded || !loaded->has_value() ||
        loaded->value().parsed().policy_revision != "1")
      return false;
    const int descriptor = ::open((root + "/valid.json").c_str(), O_WRONLY);
    if (descriptor >= 0) {
      ::close(descriptor);
      return false;
    }
    return loaded->value().parsed().entries.empty();
  });
  restricted_child([root] {
    namespace frontend = plinth::frontend;
    using nlohmann::json;
    const plinth::packages::PanelContextSchema schema{
        .version = 1,
        .fields = {{"record_id",
                    {.required = true,
                     .max_bytes = 128,
                     .meaning = "Stable fixture record identifier"}},
                   {"query",
                    {.required = false,
                     .max_bytes = 512,
                     .meaning = "Live-only search text"}}},
        .persist_keys = {"record_id"}};
    const auto loaded =
        restore::load_protected_policy(root + "/projection.json");
    if (!loaded || !loaded->has_value()) return false;
    const auto& policy = loaded->value();
    const auto approval = frontend::match_persistence_projection(
        policy, "notes", "preview", "notes:1:read", std::string(64, 'a'),
        schema);
    if (!approval ||
        approval->approved_keys != std::vector<std::string>{"record_id"})
      return false;
    const json context = {{"record_id", "fake-record-1"},
                          {"query", "unsaved search text"}};
    const auto projected =
        frontend::project_float_context(*approval, schema, context);
    if (!projected || *projected != json{{"record_id", "fake-record-1"}})
      return false;
    if (frontend::match_persistence_projection(policy, "notes", "preview",
                                               "notes:1:read",
                                               std::string(64, 'b'), schema))
      return false;
    if (frontend::project_float_context(*approval, schema,
                                        json{{"query", "missing id"}}))
      return false;
    auto malformed = context;
    malformed["record_id"] = std::string{"a\0b", 3};
    if (frontend::project_float_context(*approval, schema, malformed))
      return false;
    for (const auto name : {"missing-required.json", "wrong-meaning.json"}) {
      const auto other = restore::load_protected_policy(root + "/" + name);
      if (!other || !other->has_value() ||
          frontend::match_persistence_projection(other->value(), "notes",
                                                 "preview", "notes:1:read",
                                                 std::string(64, 'a'), schema))
        return false;
    }
    return true;
  });
  for (const auto name : {"duplicate.json", "invalid.json", "oversize.json"}) {
    restricted_child([root, name] {
      const auto loaded = restore::load_protected_policy(root + "/" + name);
      return !loaded && loaded.error() == restore::PolicyLoadError::INVALID;
    });
  }
  for (const auto name : {"link.json", "same-owner.json", "writable-file.json",
                          "writable-parent/policy.json", "hardlink.json",
                          "fifo.json", "directory.json", "valid.json/"}) {
    restricted_child([root, name] {
      const auto loaded = restore::load_protected_policy(root + "/" + name);
      return !loaded && loaded.error() == restore::PolicyLoadError::UNPROTECTED;
    });
  }
  restricted_child([root] {
    const auto loaded = restore::load_protected_policy(root + "/missing.json");
    return !loaded && loaded.error() == restore::PolicyLoadError::UNAVAILABLE;
  });
}
