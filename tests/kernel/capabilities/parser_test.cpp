#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <variant>

#include "kernel/capabilities/parser.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp"

// Unit tests for plinth::capabilities::parse_signature — pure, no PG required.
// Exercises the inverse of make_signature() and the "invalid_capability"
// error path defined by ICD-0.2.2 §Resolution Algorithm step 1.

using plinth::capabilities::CapabilityError;
using plinth::capabilities::CapabilityRegistration;
using plinth::capabilities::make_signature;
using plinth::capabilities::parse_signature;
using plinth::capabilities::ParsedSignature;

namespace {

auto parsed(std::string_view s) -> ParsedSignature {
  auto result = parse_signature(s);
  REQUIRE(std::holds_alternative<ParsedSignature>(result));
  return std::get<ParsedSignature>(result);
}

auto error(std::string_view s) -> CapabilityError {
  auto result = parse_signature(s);
  REQUIRE(std::holds_alternative<CapabilityError>(result));
  return std::get<CapabilityError>(result);
}

} // namespace

TEST_CASE("parse_signature splits canonical signatures",
          "[capabilities][parser][unit]") {
  auto p = parsed("kernel:1:db.query");
  REQUIRE(p.namespace_ == "kernel");
  REQUIRE(p.version == 1);
  REQUIRE(p.function == "db.query");

  p = parsed("terminal:1:shell");
  REQUIRE(p.namespace_ == "terminal");
  REQUIRE(p.version == 1);
  REQUIRE(p.function == "shell");

  // Multi-digit version.
  p = parsed("fs:42:read");
  REQUIRE(p.namespace_ == "fs");
  REQUIRE(p.version == 42);
  REQUIRE(p.function == "read");

  // Underscores in namespace and function.
  p = parsed("my_ext:1:read_file");
  REQUIRE(p.namespace_ == "my_ext");
  REQUIRE(p.version == 1);
  REQUIRE(p.function == "read_file");

  // Multi-dot function (logical grouping).
  p = parsed("fs:3:a.b.c.d");
  REQUIRE(p.namespace_ == "fs");
  REQUIRE(p.version == 3);
  REQUIRE(p.function == "a.b.c.d");
}

TEST_CASE("parse_signature round-trips make_signature for kernel bootstrap set",
          "[capabilities][parser][unit]") {
  // Exact set from ICD-0.2.0 §Bootstrap: Kernel Capabilities.
  struct Triple {
    std::string ns;
    int version;
    std::string function;
  };
  const std::array<Triple, 5> TABLE = {{
      {.ns = "kernel", .version = 1, .function = "db.query"},
      {.ns = "kernel", .version = 1, .function = "db.exec"},
      {.ns = "kernel", .version = 1, .function = "log"},
      {.ns = "kernel", .version = 1, .function = "audit"},
      {.ns = "kernel", .version = 1, .function = "config.get"},
  }};

  for (const auto& t : TABLE) {
    CapabilityRegistration reg{};
    reg.namespace_ = t.ns;
    reg.version = t.version;
    reg.function = t.function;

    auto sig = make_signature(reg);
    auto p = parsed(sig);
    REQUIRE(p.namespace_ == t.ns);
    REQUIRE(p.version == t.version);
    REQUIRE(p.function == t.function);
  }
}

TEST_CASE("parse_signature rejects wrong colon count",
          "[capabilities][parser][unit]") {
  REQUIRE(error("") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo:1") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo:1:bar:extra") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("a:b:c:d:e") == CapabilityError::INVALID_CAPABILITY);
}

TEST_CASE("parse_signature rejects empty segments",
          "[capabilities][parser][unit]") {
  REQUIRE(error(":1:bar") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo::bar") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo:1:") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("::") == CapabilityError::INVALID_CAPABILITY);
}

TEST_CASE("parse_signature rejects malformed version strings",
          "[capabilities][parser][unit]") {
  // Non-numeric.
  REQUIRE(error("foo:a:bar") == CapabilityError::INVALID_CAPABILITY);
  // Signed.
  REQUIRE(error("foo:-1:bar") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo:+1:bar") == CapabilityError::INVALID_CAPABILITY);
  // Decimal / scientific.
  REQUIRE(error("foo:1.0:bar") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo:1e2:bar") == CapabilityError::INVALID_CAPABILITY);
  // Whitespace.
  REQUIRE(error("foo: 1:bar") == CapabilityError::INVALID_CAPABILITY);
  REQUIRE(error("foo:1 :bar") == CapabilityError::INVALID_CAPABILITY);
  // Trailing garbage.
  REQUIRE(error("foo:1abc:bar") == CapabilityError::INVALID_CAPABILITY);
  // Hex-like prefix.
  REQUIRE(error("foo:0x1:bar") == CapabilityError::INVALID_CAPABILITY);
}

TEST_CASE("parse_signature rejects version overflow",
          "[capabilities][parser][unit]") {
  // Far outside int range.
  REQUIRE(error("foo:99999999999999999999:bar") ==
          CapabilityError::INVALID_CAPABILITY);
  // INT_MAX + 1 (on any 32-bit-or-wider int).
  REQUIRE(error("foo:2147483648:bar") == CapabilityError::INVALID_CAPABILITY);
}

TEST_CASE(
    "parse_signature returns field-specific errors for post-shape rejections",
    "[capabilities][parser][unit]") {
  // Namespace validator rejects uppercase / dashes / leading non-letter.
  REQUIRE(error("Foo:1:bar") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(error("1foo:1:bar") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(error("has-dash:1:bar") == CapabilityError::INVALID_NAMESPACE);
  REQUIRE(error(std::string(65, 'a') + ":1:bar") ==
          CapabilityError::INVALID_NAMESPACE);

  // Version validator rejects 0.
  REQUIRE(error("foo:0:bar") == CapabilityError::INVALID_VERSION);

  // Function validator rejects malformed dot patterns / uppercase / dashes.
  REQUIRE(error("foo:1:.bar") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(error("foo:1:bar.") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(error("foo:1:a..b") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(error("foo:1:Bar") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(error("foo:1:has-dash") == CapabilityError::INVALID_FUNCTION);
  REQUIRE(error("foo:1:" + std::string(129, 'a')) ==
          CapabilityError::INVALID_FUNCTION);
}

TEST_CASE("parse_signature accepts boundary-length components",
          "[capabilities][parser][unit]") {
  // Max-length namespace (64) and function (128).
  const auto MAX_NS = std::string(64, 'a');
  const auto MAX_FN = std::string(128, 'a');
  auto sig = MAX_NS + ":1:" + MAX_FN;
  auto p = parsed(sig);
  REQUIRE(p.namespace_ == MAX_NS);
  REQUIRE(p.version == 1);
  REQUIRE(p.function == MAX_FN);
}
