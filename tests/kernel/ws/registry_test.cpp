#include "kernel/ws/connection_registry.hpp"

#include <catch2/catch_test_macros.hpp>

using plinth::ws::ConnectionRegistry;
using plinth::ws::RegistryKey;

// Pure-unit tests of the registry's map semantics (no Drogon required).
// End-to-end displacement behavior is covered in auth_test.cpp.

namespace {

// A fake connection pointer whose identity matters (nullptr for simplicity
// would collapse into a shared "not set" value; using distinct int pointers
// gives us comparable identities without a real WebSocketConnection).
auto fake_conn(int id) -> drogon::WebSocketConnectionPtr {
  // Construct a shared_ptr whose identity is the bit pattern of `id` cast
  // to a pointer — gives the registry test distinct comparable identities
  // without a real WebSocketConnection. The no-op deleter is intentional;
  // there is no storage to free.
  auto* raw = reinterpret_cast<drogon::WebSocketConnection*>(
      static_cast<std::uintptr_t>(id));
  return {raw, [](drogon::WebSocketConnection*) {}};
}

} // namespace

TEST_CASE("RegistryKey equality", "[ws][registry]") {
  RegistryKey a{.auth_type = "session", .id = "abc"};
  RegistryKey b{.auth_type = "session", .id = "abc"};
  RegistryKey c{.auth_type = "pat", .id = "abc"};
  REQUIRE(a == b);
  REQUIRE_FALSE(a == c);
}

TEST_CASE("register_connection on empty slot installs", "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn = fake_conn(1);
  auto prior = reg.register_connection({.auth_type = "session", .id = "s1"},
                                       conn, nullptr);
  REQUIRE(prior == nullptr);
  REQUIRE(reg.size() == 1);
}

TEST_CASE("register_connection on occupied slot returns displaced",
          "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn1 = fake_conn(1);
  auto conn2 = fake_conn(2);
  REQUIRE(reg.register_connection({.auth_type = "session", .id = "s1"}, conn1,
                                  nullptr) == nullptr);
  auto displaced = reg.register_connection({.auth_type = "session", .id = "s1"},
                                           conn2, nullptr);
  REQUIRE(displaced == conn1);
  REQUIRE(reg.size() == 1);
}

TEST_CASE("unregister_connection ignores stale entries", "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn1 = fake_conn(1);
  auto conn2 = fake_conn(2);
  reg.register_connection({.auth_type = "session", .id = "s1"}, conn1, nullptr);
  reg.register_connection({.auth_type = "session", .id = "s1"}, conn2,
                          nullptr); // conn1 displaced

  // conn1 disconnects after being displaced — must NOT remove conn2.
  reg.unregister_connection({.auth_type = "session", .id = "s1"}, conn1);
  REQUIRE(reg.size() == 1);

  // conn2 disconnects normally — should be removed.
  reg.unregister_connection({.auth_type = "session", .id = "s1"}, conn2);
  REQUIRE(reg.size() == 0);
}

TEST_CASE("session and PAT keys with same id are distinct", "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn_s = fake_conn(1);
  auto conn_p = fake_conn(2);
  REQUIRE(reg.register_connection({.auth_type = "session", .id = "shared-id"},
                                  conn_s, nullptr) == nullptr);
  REQUIRE(reg.register_connection({.auth_type = "pat", .id = "shared-id"},
                                  conn_p, nullptr) == nullptr);
  REQUIRE(reg.size() == 2);
}

TEST_CASE("for_each sees all registered connections", "[ws][registry]") {
  ConnectionRegistry reg;
  reg.register_connection({.auth_type = "session", .id = "s1"}, fake_conn(1),
                          nullptr);
  reg.register_connection({.auth_type = "session", .id = "s2"}, fake_conn(2),
                          nullptr);
  reg.register_connection({.auth_type = "pat", .id = "p1"}, fake_conn(3),
                          nullptr);

  int count = 0;
  reg.for_each([&count](const drogon::WebSocketConnectionPtr&) { ++count; });
  REQUIRE(count == 3);
}
