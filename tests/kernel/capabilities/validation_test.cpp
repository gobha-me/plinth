#include <catch2/catch_test_macros.hpp>
#include <string>

#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp"

// Unit tests for plinth::capabilities validation — pure, no PG required.
// Exercises every rule in ICD-0.2.0 §Validation Rules.

using plinth::capabilities::CapabilityError;
using plinth::capabilities::CapabilityRegistration;
using plinth::capabilities::error_code;
using plinth::capabilities::make_signature;
using plinth::capabilities::validate_function;
using plinth::capabilities::validate_namespace;
using plinth::capabilities::validate_provider_type;
using plinth::capabilities::validate_registration;
using plinth::capabilities::validate_scope;
using plinth::capabilities::validate_version;

namespace {

auto valid_ext_reg() -> CapabilityRegistration {
  return CapabilityRegistration{
      .namespace_ = "terminal",
      .version = 1,
      .function = "shell",
      .provider_type = "extension",
      .extension_name = std::string{"terminal"},
      .scope = "instance",
      .description = "Execute a shell command",
      .rbac_rule = "terminal.shell.execute",
  };
}

auto valid_kernel_reg() -> CapabilityRegistration {
  return CapabilityRegistration{
      .namespace_ = "kernel",
      .version = 1,
      .function = "db.query",
      .provider_type = "kernel",
      .extension_name = std::nullopt,
      .scope = "instance",
      .description = "Execute a read query",
      .rbac_rule = "kernel.db.query",
  };
}

} // namespace

TEST_CASE("validate_namespace accepts conforming values",
          "[capabilities][validation][unit]") {
  REQUIRE(!validate_namespace("terminal").has_value());
  REQUIRE(!validate_namespace("fs").has_value());
  REQUIRE(!validate_namespace("my_ext").has_value());
  REQUIRE(!validate_namespace("ext9").has_value());
  REQUIRE(!validate_namespace("a").has_value());
  // Max length = 64.
  REQUIRE(!validate_namespace(std::string(64, 'a')).has_value());
}

TEST_CASE("validate_namespace rejects non-conforming values",
          "[capabilities][validation][unit]") {
  REQUIRE(validate_namespace("") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(validate_namespace("Terminal") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(validate_namespace("1abc") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(validate_namespace("_abc") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(validate_namespace("has-dash") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(validate_namespace("has.dot") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(validate_namespace(std::string(65, 'a')) ==
          CapabilityError::INVALID_NAMESPACE);
}

TEST_CASE("validate_version requires positive integer",
          "[capabilities][validation][unit]") {
  REQUIRE(!validate_version(1).has_value());
  REQUIRE(!validate_version(2).has_value());
  REQUIRE(!validate_version(99).has_value());
  REQUIRE(validate_version(0) == CapabilityError::INVALID_VERSION);
  REQUIRE(validate_version(-1) == CapabilityError::INVALID_VERSION);
}

TEST_CASE("validate_function accepts conforming names including dots",
          "[capabilities][validation][unit]") {
  REQUIRE(!validate_function("shell").has_value());
  REQUIRE(!validate_function("db.query").has_value());
  REQUIRE(!validate_function("a.b.c.d").has_value());
  REQUIRE(!validate_function("db_query").has_value());
  REQUIRE(!validate_function(std::string(128, 'a')).has_value());
}

TEST_CASE("validate_function rejects malformed dot patterns",
          "[capabilities][validation][unit]") {
  REQUIRE(validate_function("") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(validate_function(".foo") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(validate_function("foo.") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(validate_function("foo..bar") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(validate_function("Foo") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(validate_function("1foo") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(validate_function("has-dash") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(validate_function(std::string(129, 'a')) ==
          CapabilityError::INVALID_FUNCTION);
}

TEST_CASE("validate_scope accepts instance, defers user, rejects others",
          "[capabilities][validation][unit]") {
  REQUIRE(!validate_scope("instance").has_value());
  REQUIRE(validate_scope("user") == CapabilityError::USER_SCOPE_NOT_SUPPORTED);
  REQUIRE(validate_scope("") == CapabilityError::INVALID_SCOPE);
  REQUIRE(validate_scope("bogus") == CapabilityError::INVALID_SCOPE);
}

TEST_CASE("validate_provider_type accepts exactly the three values",
          "[capabilities][validation][unit]") {
  REQUIRE(!validate_provider_type("kernel").has_value());
  REQUIRE(!validate_provider_type("extension").has_value());
  REQUIRE(!validate_provider_type("sidecar").has_value());
  REQUIRE(validate_provider_type("") == CapabilityError::INVALID_PROVIDER_TYPE);
  REQUIRE(validate_provider_type("Kernel") ==
          CapabilityError::INVALID_PROVIDER_TYPE);
  REQUIRE(validate_provider_type("other") ==
          CapabilityError::INVALID_PROVIDER_TYPE);
}

TEST_CASE("validate_registration happy paths",
          "[capabilities][validation][unit]") {
  REQUIRE(!validate_registration(valid_ext_reg()).has_value());
  REQUIRE(!validate_registration(valid_kernel_reg()).has_value());
}

TEST_CASE("validate_registration rejects missing extension_name for extension",
          "[capabilities][validation][unit]") {
  auto reg = valid_ext_reg();
  reg.extension_name = std::nullopt;
  REQUIRE(validate_registration(reg) ==
          CapabilityError::MISSING_EXTENSION_NAME);
  reg.extension_name = std::string{};
  REQUIRE(validate_registration(reg) ==
          CapabilityError::MISSING_EXTENSION_NAME);
}

TEST_CASE("validate_registration rejects missing extension_name for sidecar",
          "[capabilities][validation][unit]") {
  auto reg = valid_ext_reg();
  reg.provider_type = "sidecar";
  reg.extension_name = std::nullopt;
  REQUIRE(validate_registration(reg) ==
          CapabilityError::MISSING_EXTENSION_NAME);
}

TEST_CASE("validate_registration reserves kernel namespace",
          "[capabilities][validation][unit]") {
  auto reg = valid_ext_reg();
  reg.namespace_ = "kernel";
  reg.rbac_rule = "kernel.something";
  REQUIRE(validate_registration(reg) == CapabilityError::RESERVED_NAMESPACE);
}

TEST_CASE("validate_registration requires rule namespace to match",
          "[capabilities][validation][unit]") {
  auto reg = valid_ext_reg();
  reg.rbac_rule = "other.shell.execute";
  REQUIRE(validate_registration(reg) == CapabilityError::NAMESPACE_MISMATCH);

  // Rule equal to the prefix but no action part is also invalid.
  reg.rbac_rule = "terminal";
  REQUIRE(validate_registration(reg) == CapabilityError::NAMESPACE_MISMATCH);

  reg.rbac_rule = "terminal.";
  REQUIRE(validate_registration(reg) == CapabilityError::NAMESPACE_MISMATCH);
}

TEST_CASE("validate_registration enforces description length",
          "[capabilities][validation][unit]") {
  auto reg = valid_kernel_reg();
  reg.description = std::string(257, 'x');
  REQUIRE(validate_registration(reg) == CapabilityError::INVALID_DESCRIPTION);

  reg.description = std::string(256, 'x');
  REQUIRE(!validate_registration(reg).has_value());
}

TEST_CASE("validate_registration propagates field-level errors",
          "[capabilities][validation][unit]") {
  auto reg = valid_ext_reg();
  reg.namespace_ = "Bad";
  REQUIRE(validate_registration(reg) == CapabilityError::INVALID_NAMESPACE);

  reg = valid_ext_reg();
  reg.version = 0;
  REQUIRE(validate_registration(reg) == CapabilityError::INVALID_VERSION);

  reg = valid_ext_reg();
  reg.function = "Bad.";
  REQUIRE(validate_registration(reg) == CapabilityError::INVALID_FUNCTION);

  reg = valid_ext_reg();
  reg.scope = "user";
  REQUIRE(validate_registration(reg) ==
          CapabilityError::USER_SCOPE_NOT_SUPPORTED);
}

TEST_CASE("make_signature composes namespace:version:function",
          "[capabilities][validation][unit]") {
  CapabilityRegistration reg{};
  reg.namespace_ = "terminal";
  reg.version = 1;
  reg.function = "shell";
  REQUIRE(make_signature(reg) == "terminal:1:shell");

  reg.namespace_ = "kernel";
  reg.version = 2;
  reg.function = "db.query";
  REQUIRE(make_signature(reg) == "kernel:2:db.query");
}

TEST_CASE("error_code returns snake_case strings for every value",
          "[capabilities][validation][unit]") {
  REQUIRE(error_code(CapabilityError::INVALID_NAMESPACE) ==
          "invalid_namespace");
  REQUIRE(error_code(CapabilityError::INVALID_VERSION) == "invalid_version");
  REQUIRE(error_code(CapabilityError::INVALID_FUNCTION) == "invalid_function");
  REQUIRE(error_code(CapabilityError::INVALID_SCOPE) == "invalid_scope");
  REQUIRE(error_code(CapabilityError::INVALID_PROVIDER_TYPE) ==
          "invalid_provider_type");
  REQUIRE(error_code(CapabilityError::INVALID_DESCRIPTION) ==
          "invalid_description");
  REQUIRE(error_code(CapabilityError::INVALID_CAPABILITY) ==
          "invalid_capability");
  REQUIRE(error_code(CapabilityError::MISSING_EXTENSION_NAME) ==
          "missing_extension_name");
  REQUIRE(error_code(CapabilityError::RESERVED_NAMESPACE) ==
          "reserved_namespace");
  REQUIRE(error_code(CapabilityError::NAMESPACE_MISMATCH) ==
          "namespace_mismatch");
  REQUIRE(error_code(CapabilityError::CAPABILITY_EXISTS) ==
          "capability_exists");
  REQUIRE(error_code(CapabilityError::CAPABILITY_NOT_FOUND) ==
          "capability_not_found");
  REQUIRE(error_code(CapabilityError::RBAC_RULE_NOT_FOUND) ==
          "rbac_rule_not_found");
  REQUIRE(error_code(CapabilityError::USER_SCOPE_NOT_SUPPORTED) ==
          "user_scope_not_supported");
  REQUIRE(error_code(CapabilityError::DB_ERROR) == "db_error");
}
